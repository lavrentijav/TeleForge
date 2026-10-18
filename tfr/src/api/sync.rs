//! The `sync.*` operations, on top of [`crate::store`].
//!
//! # Single owner, no async runtime
//!
//! `docs/architecture.md` § 4 requires exactly one owner of a tenant's `data.db`
//! (finding C5 was one `sqlite3*` touched from a callback thread). The transport is
//! a thread-per-connection HTTP listener, so "one owner" is expressed as a
//! [`std::sync::Mutex`] around the map of open stores: every handler takes the lock,
//! and a store is never used from two threads at once. That is stricter than needed
//! for reads (WAL would allow concurrent readers) and it is what keeps the write
//! path honest — the alternative is a lock per store plus a promise that every
//! writer remembers to take it.
//!
//! # Errors are values, never panics
//!
//! Every function here returns [`Result`]. There is no `unwrap` on the request path:
//! malformed input becomes [`Error::Invalid`], an inconsistent cursor becomes
//! [`Error::Conflict`], and a SQLite failure becomes [`Error::Storage`]. The HTTP
//! layer maps those three to 400/409/500 respectively.

use std::collections::BTreeMap;
use std::sync::Mutex;

use crate::error::{Error, Result};
use crate::store::conn::Store;
use crate::store::event::AccountId;
use crate::store::query::{self, AccountStamp, ConflictRow, StoredOp};
use crate::store::retention::{self, PurgeReport, RetentionPolicy};
use crate::store::{self, Event, IngestOutcome};

/// Default and maximum page size for `sync.pull` / `sync.conflicts`.
///
/// The client asks; the agent decides. `docs/architecture.md` § 10.5: `PageSpec`
/// default 500, ceiling 2000 — the same ceiling as `MAX_BATCH_EVENTS`, so one page
/// out is never bigger than one batch in.
pub const DEFAULT_PAGE_LIMIT: u32 = 200;
pub const MAX_PAGE_LIMIT: u32 = crate::store::MAX_BATCH_EVENTS as u32;

/// One page of `sync.pull`.
#[derive(Clone, Debug)]
pub struct PullPage {
    pub since_seq: u64,
    /// Cursor to send next: the last delivered `op_seq`, or `since_seq` when the
    /// page is empty.
    pub next_seq: u64,
    pub high_water: u64,
    pub has_more: bool,
    /// Highest pruned `op_seq` for this account, when the account has been pruned.
    pub pruned_upto: Option<u64>,
    pub ops: Vec<StoredOp>,
}

/// What `sync.push` reports: per-event verdicts plus the resulting stamp.
#[derive(Clone, Debug)]
pub struct PushOutcome {
    pub outcome: IngestOutcome,
    pub stamp: AccountStamp,
}

/// Owns the open stores and the retention policy.
pub struct SyncService {
    data_dir: String,
    retention: RetentionPolicy,
    stores: Mutex<BTreeMap<String, Store>>,
}

impl SyncService {
    pub fn new(data_dir: impl Into<String>, retention: RetentionPolicy) -> Self {
        Self {
            data_dir: data_dir.into(),
            retention,
            stores: Mutex::new(BTreeMap::new()),
        }
    }

    pub fn data_dir(&self) -> &str {
        &self.data_dir
    }

    pub fn retention(&self) -> RetentionPolicy {
        self.retention
    }

    /// Run `body` with the tenant's store under the single-owner lock.
    ///
    /// A poisoned mutex is reported as a storage error rather than propagated as a
    /// panic: a panic in one request must not take the listener down, and a
    /// poisoned lock still leaves the SQLite file consistent (every write is in a
    /// transaction).
    pub fn with_store<T>(
        &self,
        tenant_hex: &str,
        body: impl FnOnce(&mut Store) -> Result<T>,
    ) -> Result<T> {
        // A tenant id is exactly 16 bytes of hex. Anything else is a caller bug or a
        // path-traversal attempt ("../x" is not hex, but "0011" is), so the length
        // check is the one that matters.
        if tenant_hex.len() != 32 || !tenant_hex.bytes().all(|byte| byte.is_ascii_hexdigit()) {
            return Err(Error::Invalid(format!(
                "tenant {tenant_hex:?} must be lowercase hex"
            )));
        }
        let mut stores = self
            .stores
            .lock()
            .map_err(|_| Error::Storage("tenant store lock is poisoned".to_string()))?;
        if !stores.contains_key(tenant_hex) {
            let store = store::open_for_tenant(&self.data_dir, tenant_hex)?;
            stores.insert(tenant_hex.to_string(), store);
        }
        let store = stores
            .get_mut(tenant_hex)
            .ok_or_else(|| Error::Storage("tenant store disappeared".to_string()))?;
        body(store)
    }

    /// Apply a parsed batch. The caller has already authenticated `account` and
    /// derived `device_id` from the token — a client cannot claim another device's
    /// clock.
    pub fn push(
        &self,
        tenant_hex: &str,
        account: &AccountId,
        events: &[Event],
        now_ms: i64,
    ) -> Result<PushOutcome> {
        self.with_store(tenant_hex, |store| {
            let outcome = store::ingest_batch_at(store, account, events, now_ms)?;
            let stamp = query::account_stamp(store, account)?;
            Ok(PushOutcome { outcome, stamp })
        })
    }

    pub fn pull(
        &self,
        tenant_hex: &str,
        account: &AccountId,
        since_seq: u64,
        limit: u32,
    ) -> Result<PullPage> {
        self.with_store(tenant_hex, |store| {
            let limit = limit.clamp(1, MAX_PAGE_LIMIT);
            let stamp = query::account_stamp(store, account)?;
            if since_seq > stamp.high_water {
                // The client claims to have applied operations this store never
                // issued. Either it is talking about a different database or the
                // store was restored from an older backup; both need a resync, and
                // answering "empty page" would look like "you are up to date".
                return Err(Error::Conflict(format!(
                    "since_seq {since_seq} is beyond high_water {}",
                    stamp.high_water
                )));
            }
            let pruned_upto = retention::pruned_upto(store, account)?;
            if let Some(floor) = pruned_upto {
                if since_seq < floor {
                    return Err(Error::Conflict(format!(
                        "history up to op_seq {floor} has been pruned; re-sync from {floor}"
                    )));
                }
            }
            let ops = query::ops_since(store, account, since_seq, limit)?;
            let next_seq = ops.last().map(|op| op.op_seq).unwrap_or(since_seq);
            Ok(PullPage {
                since_seq,
                next_seq,
                high_water: stamp.high_water,
                has_more: next_seq < stamp.high_water,
                pruned_upto,
                ops,
            })
        })
    }

    pub fn stamp(&self, tenant_hex: &str, account: &AccountId) -> Result<AccountStamp> {
        self.with_store(tenant_hex, |store| query::account_stamp(store, account))
    }

    pub fn conflicts(
        &self,
        tenant_hex: &str,
        account: &AccountId,
        since_id: u64,
        limit: u32,
        include_resolved: bool,
    ) -> Result<Vec<ConflictRow>> {
        self.with_store(tenant_hex, |store| {
            query::conflict_list(
                store,
                account,
                since_id,
                limit.clamp(1, MAX_PAGE_LIMIT),
                include_resolved,
            )
        })
    }

    pub fn resolve_conflict(
        &self,
        tenant_hex: &str,
        account: &AccountId,
        conflict_id: u64,
        resolved_by: &str,
    ) -> Result<bool> {
        self.with_store(tenant_hex, |store| {
            query::resolve_conflict(store, account, conflict_id, resolved_by)
        })
    }

    pub fn prune(
        &self,
        tenant_hex: &str,
        account: &AccountId,
        upto_seq: u64,
    ) -> Result<(PurgeReport, AccountStamp)> {
        self.with_store(tenant_hex, |store| {
            let report = retention::prune_upto(store, account, upto_seq)?;
            let stamp = query::account_stamp(store, account)?;
            Ok((report, stamp))
        })
    }

    /// Run the configured retention policy over every tenant that has been opened
    /// so far, plus every tenant directory on disk.
    ///
    /// Returns one report per tenant so the caller can log what happened. An error
    /// for one tenant does not stop the others — a retention run that aborts on the
    /// first bad file never purges anything again.
    pub fn run_retention(&self, now_ms: i64) -> Vec<(String, Result<PurgeReport>)> {
        let mut tenants = self.tenants_on_disk();
        if let Ok(stores) = self.stores.lock() {
            for tenant in stores.keys() {
                if !tenants.contains(tenant) {
                    tenants.push(tenant.clone());
                }
            }
        }
        let mut reports = Vec::with_capacity(tenants.len());
        for tenant in tenants {
            let report = self.with_store(&tenant, |store| self.retention.apply(store, now_ms));
            reports.push((tenant, report));
        }
        reports
    }

    fn tenants_on_disk(&self) -> Vec<String> {
        let root = format!("{}/tenants", self.data_dir);
        let mut out = Vec::new();
        let Ok(entries) = std::fs::read_dir(&root) else {
            return out;
        };
        for entry in entries.flatten() {
            if !entry.file_type().map(|kind| kind.is_dir()).unwrap_or(false) {
                continue;
            }
            let Some(name) = entry.file_name().to_str().map(str::to_string) else {
                continue;
            };
            if name.len() == 32 && name.bytes().all(|byte| byte.is_ascii_hexdigit()) {
                out.push(name);
            }
        }
        out.sort();
        out
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::store::{Event, EventKind};
    use crate::sync::clock::Hlc;
    use crate::test_support::TestDir;

    const TENANT: &str = "00112233445566778899aabbccddeeff";

    fn account() -> AccountId {
        AccountId::new([0x51; 16]).unwrap()
    }

    fn service() -> (TestDir, SyncService) {
        let directory = TestDir::new("sync-service");
        let service = SyncService::new(
            directory.path().to_string_lossy().to_string(),
            RetentionPolicy::default_for_vps(),
        );
        (directory, service)
    }

    fn profile(hlc_ms: u64, value: &[u8]) -> Event {
        Event::new(
            account(),
            "dev-a",
            EventKind::ProfileUpdate,
            "profile:900",
            Some("username"),
            value.to_vec(),
            Hlc::new(hlc_ms, 0),
        )
    }

    #[test]
    fn push_then_pull_roundtrips() {
        let (_directory, service) = service();
        let events = vec![profile(1_000, b"@neo"), profile(1_001, b"@trinity")];
        let pushed = service.push(TENANT, &account(), &events, 5_000).unwrap();
        assert_eq!(pushed.outcome.applied, 2);
        assert_eq!(pushed.stamp.high_water, 2);
        assert_eq!(pushed.stamp.op_count, 2);

        let page = service.pull(TENANT, &account(), 0, 10).unwrap();
        assert_eq!(page.ops.len(), 2);
        assert_eq!(page.next_seq, 2);
        assert!(!page.has_more);
        assert_eq!(page.ops[0].value, b"@neo");
        assert_eq!(page.ops[1].hlc, Hlc::new(1_001, 0));

        // A second pull from the cursor returns nothing new.
        let empty = service.pull(TENANT, &account(), page.next_seq, 10).unwrap();
        assert!(empty.ops.is_empty());
        assert_eq!(empty.next_seq, 2);
        assert!(!empty.has_more);
    }

    #[test]
    fn pagination_splits_the_page_and_reports_more() {
        let (_directory, service) = service();
        let events: Vec<Event> = (0..7).map(|index| profile(1_000 + index, b"x")).collect();
        service.push(TENANT, &account(), &events, 5_000).unwrap();
        let first = service.pull(TENANT, &account(), 0, 3).unwrap();
        assert_eq!(first.ops.len(), 3);
        assert_eq!(first.next_seq, 3);
        assert!(first.has_more);
        let second = service.pull(TENANT, &account(), first.next_seq, 3).unwrap();
        assert_eq!(second.ops.len(), 3);
        assert_eq!(second.next_seq, 6);
        assert!(second.has_more);
        let third = service
            .pull(TENANT, &account(), second.next_seq, 3)
            .unwrap();
        assert_eq!(third.ops.len(), 1);
        assert!(!third.has_more);
        // A silly limit is clamped, not obeyed.
        let clamped = service.pull(TENANT, &account(), 0, u32::MAX).unwrap();
        assert_eq!(clamped.ops.len(), 7);
        let zero = service.pull(TENANT, &account(), 0, 0).unwrap();
        assert_eq!(zero.ops.len(), 1, "limit 0 is clamped to one, not to zero");
    }

    #[test]
    fn a_future_cursor_is_a_conflict_not_an_empty_page() {
        let (_directory, service) = service();
        service
            .push(TENANT, &account(), &[profile(1_000, b"x")], 5_000)
            .unwrap();
        let error = service.pull(TENANT, &account(), 42, 10).unwrap_err();
        assert!(matches!(error, Error::Conflict(_)), "{error}");
        assert!(error.to_string().contains("beyond high_water"));
    }

    #[test]
    fn a_cursor_below_the_pruned_floor_is_refused_with_the_floor_in_the_message() {
        let (_directory, service) = service();
        let events: Vec<Event> = (0..5).map(|index| profile(1_000 + index, b"x")).collect();
        service.push(TENANT, &account(), &events, 5_000).unwrap();
        let (report, stamp) = service.prune(TENANT, &account(), 3).unwrap();
        assert_eq!(report.ops, 3);
        assert_eq!(stamp.high_water, 5);
        let error = service.pull(TENANT, &account(), 0, 10).unwrap_err();
        assert!(matches!(error, Error::Conflict(_)), "{error}");
        assert!(error.to_string().contains("pruned"));
        // Resuming from the floor works.
        let page = service.pull(TENANT, &account(), 3, 10).unwrap();
        assert_eq!(page.ops.len(), 2);
        assert_eq!(page.pruned_upto, Some(3));
    }

    #[test]
    fn tenants_are_isolated_in_separate_files() {
        let (_directory, service) = service();
        let other_tenant = "ffeeddccbbaa99887766554433221100";
        service
            .push(TENANT, &account(), &[profile(1_000, b"x")], 5_000)
            .unwrap();
        service
            .push(other_tenant, &account(), &[profile(1_000, b"x")], 5_000)
            .unwrap();
        assert_eq!(
            service.pull(TENANT, &account(), 0, 10).unwrap().ops.len(),
            1
        );
        assert_eq!(
            service
                .pull(other_tenant, &account(), 0, 10)
                .unwrap()
                .ops
                .len(),
            1
        );
        // Same event, two tenants: both stored, because the files are separate.
        assert_eq!(
            service.pull(TENANT, &account(), 0, 10).unwrap().high_water,
            1
        );
    }

    #[test]
    fn an_invalid_tenant_id_is_rejected_before_touching_the_filesystem() {
        let (_directory, service) = service();
        for bad in ["", "../escape", "zzzz", "0011"] {
            let error = service.stamp(bad, &account()).unwrap_err();
            assert!(matches!(error, Error::Invalid(_)), "{bad:?}: {error}");
        }
    }

    #[test]
    fn retention_runs_over_every_tenant_and_reports_per_tenant() {
        let (_directory, service) = service();
        let old = profile(1_000, b"old");
        service.push(TENANT, &account(), &[old], 1_000).unwrap();
        // 100 days later the 90-day policy drops it.
        let reports = service.run_retention(100 * 24 * 60 * 60 * 1_000);
        assert_eq!(reports.len(), 1);
        let (tenant, report) = &reports[0];
        assert_eq!(tenant, TENANT);
        assert_eq!(report.as_ref().unwrap().ops, 1);
        assert_eq!(service.stamp(TENANT, &account()).unwrap().op_count, 0);
    }

    #[test]
    fn conflicts_round_trip_through_the_service() {
        let (_directory, service) = service();
        let newer = Event::new(
            account(),
            "dev-a",
            EventKind::ProfileUpdate,
            "profile:900",
            Some("username"),
            b"@new".to_vec(),
            Hlc::new(2_000, 0),
        );
        service.push(TENANT, &account(), &[newer], 5_000).unwrap();
        let older = Event::new(
            account(),
            "dev-b",
            EventKind::ProfileUpdate,
            "profile:900",
            Some("username"),
            b"@old".to_vec(),
            Hlc::new(1_000, 0),
        );
        let pushed = service.push(TENANT, &account(), &[older], 5_001).unwrap();
        assert_eq!(pushed.outcome.conflicts, 1);

        let conflicts = service.conflicts(TENANT, &account(), 0, 10, true).unwrap();
        assert_eq!(conflicts.len(), 1);
        assert!(service
            .resolve_conflict(TENANT, &account(), conflicts[0].conflict_id, "desktop")
            .unwrap());
        let resolved = service.conflicts(TENANT, &account(), 0, 10, true).unwrap();
        assert_eq!(resolved[0].resolved_by.as_deref(), Some("desktop"));
        assert!(!service
            .resolve_conflict(TENANT, &account(), 999, "desktop")
            .unwrap());
    }

    #[test]
    fn the_same_store_is_reused_across_calls() {
        let (_directory, service) = service();
        service
            .push(TENANT, &account(), &[profile(1_000, b"x")], 5_000)
            .unwrap();
        let open = service.stores.lock().unwrap().len();
        assert_eq!(open, 1);
        service
            .push(TENANT, &account(), &[profile(1_001, b"y")], 5_001)
            .unwrap();
        assert_eq!(service.stores.lock().unwrap().len(), 1);
        assert_eq!(service.stamp(TENANT, &account()).unwrap().high_water, 2);
    }
}
