//! Retention.
//!
//! # Why this exists
//!
//! Audit H3: the C++ side had a 30-day purge for `OnlineEvent` that was called
//! from exactly two places (startup and a settings slider) and an index that did
//! not match the purge query, so in practice the tables grew forever, and
//! `PeerArchiveName`/`Username`/`Bio` had no retention at all.
//!
//! Two independent knobs, because they answer different questions:
//!
//! * [`purge_by_age`] — "a presence observation older than N days is not worth
//!   keeping". This is the privacy knob.
//! * [`purge_by_size`] — "this VPS has 2 GiB"; keeps the newest rows per account
//!   and then trims the journal until the estimated size fits. This is the
//!   capacity knob.
//!
//! Both are explicit calls (from `serve`'s periodic task and from `self-test`),
//! never a `Drop`. A retention run that fails must be visible, not swallowed.
//!
//! # What retention does not do
//!
//! It does not decrement `entities.revision_count` or reset `accounts.high_water`.
//! Both are monotone by design: a cursor a client already holds must stay valid,
//! and "we deleted some old rows" is not a rollback. If that were wrong, an old
//! device pulling from `since_seq` would silently miss operations instead of
//! getting an explicit `1000` from the API layer.

use rusqlite::OptionalExtension;

use crate::error::Result;
use crate::store::conn::Store;
use crate::store::event::AccountId;

/// Per-table deletion counters.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct PurgeReport {
    pub ops: u64,
    pub presence: u64,
    pub profiles: u64,
    pub archive: u64,
}

impl PurgeReport {
    pub fn total(&self) -> u64 {
        self.ops + self.presence + self.profiles + self.archive
    }

    pub fn is_empty(&self) -> bool {
        self.total() == 0
    }

    pub fn to_json(&self) -> String {
        format!(
            "{{\"ops\":{},\"presence\":{},\"profiles\":{},\"archive\":{},\"total\":{}}}",
            self.ops,
            self.presence,
            self.profiles,
            self.archive,
            self.total()
        )
    }

    fn add(&mut self, other: PurgeReport) {
        self.ops += other.ops;
        self.presence += other.presence;
        self.profiles += other.profiles;
        self.archive += other.archive;
    }
}

/// Declarative retention. Every field is optional; `None` means "leave alone".
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct RetentionPolicy {
    /// Rows whose observation time is older than this are deleted.
    pub max_age_ms: Option<i64>,
    /// Keep at most this many journal rows per account, newest first.
    pub max_ops_per_account: Option<u64>,
    /// Trim the oldest rows until the estimated size fits this many bytes.
    pub max_bytes: Option<u64>,
}

impl RetentionPolicy {
    /// The default for a small VPS: 90 days of history, no size cap.
    pub fn default_for_vps() -> Self {
        Self {
            max_age_ms: Some(90 * 24 * 60 * 60 * 1_000),
            max_ops_per_account: None,
            max_bytes: None,
        }
    }

    /// Run every configured step, oldest-first. Returns the sum of the reports.
    pub fn apply(&self, store: &mut Store, now_ms: i64) -> Result<PurgeReport> {
        let mut report = PurgeReport::default();
        if let Some(max_age_ms) = self.max_age_ms {
            let cutoff = now_ms.saturating_sub(max_age_ms);
            report.add(purge_by_age(store, cutoff)?);
        }
        if self.max_ops_per_account.is_some() || self.max_bytes.is_some() {
            report.add(purge_by_size(
                store,
                self.max_ops_per_account,
                self.max_bytes,
            )?);
        }
        Ok(report)
    }
}

/// Delete everything observed strictly before `cutoff_ms`.
///
/// Rows are deleted per table by the timestamp that table is queried by, so a
/// partially purged store is still internally consistent: an op and its
/// projection row carry the same `op_seq` and therefore the same observation
/// time, so they are removed together.
pub fn purge_by_age(store: &mut Store, cutoff_ms: i64) -> Result<PurgeReport> {
    store.transaction(|tx| {
        let connection = tx.connection();
        let ops = connection.execute("DELETE FROM ops WHERE applied_at_ms < ?1", [cutoff_ms])?;
        let presence =
            connection.execute("DELETE FROM presence WHERE observed_ms < ?1", [cutoff_ms])?;
        let profiles =
            connection.execute("DELETE FROM profiles WHERE observed_ms < ?1", [cutoff_ms])?;
        let archive = connection.execute(
            "DELETE FROM archive_obs WHERE observed_ms < ?1",
            [cutoff_ms],
        )?;
        Ok(PurgeReport {
            ops: ops as u64,
            presence: presence as u64,
            profiles: profiles as u64,
            archive: archive as u64,
        })
    })
}

/// Trim by size: first the per-account row cap, then the total byte budget.
pub fn purge_by_size(
    store: &mut Store,
    max_ops_per_account: Option<u64>,
    max_bytes: Option<u64>,
) -> Result<PurgeReport> {
    let mut report = PurgeReport::default();
    if let Some(cap) = max_ops_per_account {
        report.add(trim_per_account(store, cap)?);
    }
    if let Some(budget) = max_bytes {
        report.add(trim_to_bytes(store, budget)?);
    }
    Ok(report)
}

/// Delete one account's history up to and including `upto_op_seq`.
///
/// This is the client-driven variant of the cap: the client says "I have applied
/// everything up to N, forget it". Two rules make it safe to expose:
///
/// * `upto_op_seq` may not exceed `accounts.high_water`. A larger value means the
///   caller is talking about a different (restored? rolled back?) store, which is a
///   409, not a delete — accepting it would silently wipe rows that do exist.
/// * `high_water` itself is **not** moved. A device that still holds a cursor below
///   the pruned range must get "these ops are gone" from `sync.pull`
///   (`since_seq` below `pruned_upto`), not a silently shorter answer. The pruned
///   boundary is recorded in `schema_meta` as `pruned_upto:<account_hex>` so the
///   API can tell the difference between "never existed" and "pruned".
///
/// Conflicts are **not** pruned with the journal: they are the record of *decisions
/// this store could not make*, they carry their own copy of both HLCs and devices,
/// and a pruned op_seq inside them is a reference to history that the client already
/// acknowledged. `sync.conflicts` therefore keeps working after a prune.
pub fn prune_upto(store: &mut Store, account: &AccountId, upto_op_seq: u64) -> Result<PurgeReport> {
    let report = store.transaction(|tx| {
        let high_water = tx.high_water(account)?;
        if upto_op_seq > high_water {
            return Err(crate::error::Error::Conflict(format!(
                "cannot prune up to {upto_op_seq}: the account's high_water is {high_water}"
            )));
        }
        delete_up_to_op_seq(tx, account, upto_op_seq as i64)
    })?;
    // Recorded outside the delete transaction on purpose: the delete is the
    // authoritative act, and a crash between the two must not leave the marker
    // claiming a prune that did not happen.
    if !report.is_empty() {
        store.transaction(|tx| {
            tx.connection().execute(
                "INSERT INTO schema_meta (key, value) VALUES (?1, ?2)
                 ON CONFLICT (key) DO UPDATE SET value = excluded.value",
                rusqlite::params![
                    format!("pruned_upto:{}", account.to_hex()),
                    upto_op_seq.to_string()
                ],
            )?;
            Ok(())
        })?;
    }
    Ok(report)
}

/// The highest `op_seq` ever pruned for this account, if any.
///
/// `sync.pull` uses it to answer a cursor below the pruned floor with an explicit
/// conflict instead of an empty page that the client would read as "you are up to
/// date" — the failure mode a silent prune would create.
pub fn pruned_upto(store: &Store, account: &AccountId) -> Result<Option<u64>> {
    let value: Option<String> = store
        .connection()
        .query_row(
            "SELECT value FROM schema_meta WHERE key = ?1",
            [format!("pruned_upto:{}", account.to_hex())],
            |row| row.get(0),
        )
        .optional()?;
    Ok(value.and_then(|text| text.parse::<u64>().ok()))
}

/// Keep at most `cap` journal rows per account.
///
/// The boundary is an `op_seq`, not a timestamp: `op_seq` is unique per account
/// and monotone, so the newest `cap` rows are unambiguous even if two devices
/// wrote in the same millisecond.
fn trim_per_account(store: &mut Store, cap: u64) -> Result<PurgeReport> {
    let accounts = store.transaction(|tx| {
        let mut statement = tx
            .connection()
            .prepare("SELECT account_id FROM accounts ORDER BY account_id ASC")?;
        let rows = statement.query_map([], |row| row.get::<_, Vec<u8>>(0))?;
        let mut out = Vec::new();
        for row in rows {
            out.push(crate::store::event::AccountId::from_slice(&row?)?);
        }
        Ok(out)
    })?;
    let mut report = PurgeReport::default();
    for account in accounts {
        let boundary: Option<i64> = store.transaction(|tx| {
            let value = tx
                .connection()
                .query_row(
                    "SELECT op_seq FROM ops WHERE account_id = ?1
                      ORDER BY op_seq DESC LIMIT 1 OFFSET ?2",
                    rusqlite::params![account.as_blob(), cap as i64],
                    |row| row.get(0),
                )
                .ok();
            Ok(value)
        })?;
        let Some(boundary) = boundary else {
            continue; // the account is already at or below the cap
        };
        let deleted = store.transaction(|tx| delete_up_to_op_seq(tx, &account, boundary))?;
        report.add(deleted);
    }
    Ok(report)
}

/// Delete everything at or below `boundary_op_seq` for one account, journal and
/// projections together.
fn delete_up_to_op_seq(
    tx: &crate::store::conn::Tx<'_>,
    account: &crate::store::event::AccountId,
    boundary_op_seq: i64,
) -> Result<PurgeReport> {
    let connection = tx.connection();
    let ops = connection.execute(
        "DELETE FROM ops WHERE account_id = ?1 AND op_seq <= ?2",
        rusqlite::params![account.as_blob(), boundary_op_seq],
    )?;
    let presence = connection.execute(
        "DELETE FROM presence WHERE account_id = ?1 AND op_seq <= ?2",
        rusqlite::params![account.as_blob(), boundary_op_seq],
    )?;
    let profiles = connection.execute(
        "DELETE FROM profiles WHERE account_id = ?1 AND op_seq <= ?2",
        rusqlite::params![account.as_blob(), boundary_op_seq],
    )?;
    let archive = connection.execute(
        "DELETE FROM archive_obs WHERE account_id = ?1 AND op_seq <= ?2",
        rusqlite::params![account.as_blob(), boundary_op_seq],
    )?;
    Ok(PurgeReport {
        ops: ops as u64,
        presence: presence as u64,
        profiles: profiles as u64,
        archive: archive as u64,
    })
}

/// Delete the oldest rows until the estimated on-disk size is under `budget`.
///
/// The estimate is `SUM(LENGTH(value)) + 64 * COUNT(*)` over the four tables:
/// the payloads dominate, and the 64-byte term accounts for the per-row headers
/// and index entries without pretending to be exact. `EXPLAIN`-grade accuracy is
/// not needed — what matters is that the knob moves in the right direction and
/// that the newest rows are the ones kept.
fn trim_to_bytes(store: &mut Store, budget: u64) -> Result<PurgeReport> {
    let mut report = PurgeReport::default();
    // One round selects the rows that cover the excess, up to this many; a huge
    // excess takes several rounds so no single transaction holds the write lock
    // for a whole-table scan.
    const MAX_VICTIMS_PER_ROUND: usize = 4_096;
    loop {
        let used = estimated_bytes(store)?;
        if used <= budget {
            break;
        }
        let excess = used - budget;
        let victims = store.transaction(|tx| select_victims(tx, excess, MAX_VICTIMS_PER_ROUND))?;
        if victims.is_empty() {
            // Nothing left to delete: the budget cannot be met (fixed overhead).
            break;
        }
        let deleted = store.transaction(|tx| delete_victims(tx, &victims))?;
        report.add(deleted);
        if deleted.total() == 0 {
            break;
        }
    }
    Ok(report)
}

/// Walk the journal oldest-first and collect `(account_id, op_seq)` until the
/// accumulated payload covers `excess` bytes.
fn select_victims(
    tx: &crate::store::conn::Tx<'_>,
    excess: u64,
    max_victims: usize,
) -> Result<Vec<(Vec<u8>, i64)>> {
    let mut statement = tx.connection().prepare(
        "SELECT account_id, op_seq, LENGTH(value) + 64
           FROM ops ORDER BY applied_at_ms ASC, op_seq ASC",
    )?;
    let mut rows = statement.query([])?;
    let mut victims = Vec::new();
    let mut collected = 0u64;
    while collected < excess && victims.len() < max_victims {
        let Some(row) = rows.next()? else { break };
        let account: Vec<u8> = row.get(0)?;
        let op_seq: i64 = row.get(1)?;
        let size: i64 = row.get(2)?;
        collected = collected.saturating_add(size.max(0) as u64);
        victims.push((account, op_seq));
    }
    Ok(victims)
}

fn delete_victims(
    tx: &crate::store::conn::Tx<'_>,
    victims: &[(Vec<u8>, i64)],
) -> Result<PurgeReport> {
    let connection = tx.connection();
    connection.execute_batch(
        "CREATE TEMP TABLE IF NOT EXISTS purge_keys (
             account_id BLOB NOT NULL,
             op_seq     INTEGER NOT NULL
         );
         DELETE FROM purge_keys;",
    )?;
    {
        let mut insert = connection
            .prepare("INSERT OR IGNORE INTO purge_keys (account_id, op_seq) VALUES (?1, ?2)")?;
        for (account, op_seq) in victims {
            insert.execute(rusqlite::params![account, op_seq])?;
        }
    }
    let presence = connection.execute(
        "DELETE FROM presence WHERE EXISTS (
             SELECT 1 FROM purge_keys AS k
              WHERE k.account_id = presence.account_id
                AND k.op_seq = presence.op_seq)",
        [],
    )?;
    let profiles = connection.execute(
        "DELETE FROM profiles WHERE EXISTS (
             SELECT 1 FROM purge_keys AS k
              WHERE k.account_id = profiles.account_id
                AND k.op_seq = profiles.op_seq)",
        [],
    )?;
    let archive = connection.execute(
        "DELETE FROM archive_obs WHERE EXISTS (
             SELECT 1 FROM purge_keys AS k
              WHERE k.account_id = archive_obs.account_id
                AND k.op_seq = archive_obs.op_seq)",
        [],
    )?;
    let ops = connection.execute(
        "DELETE FROM ops WHERE EXISTS (
             SELECT 1 FROM purge_keys AS k
              WHERE k.account_id = ops.account_id
                AND k.op_seq = ops.op_seq)",
        [],
    )?;
    connection.execute_batch("DELETE FROM purge_keys;")?;
    Ok(PurgeReport {
        ops: ops as u64,
        presence: presence as u64,
        profiles: profiles as u64,
        archive: archive as u64,
    })
}

/// Estimated bytes held by the reclaimable tables.
pub fn estimated_bytes(store: &Store) -> Result<u64> {
    let mut total = 0u64;
    for sql in [
        "SELECT COALESCE(SUM(LENGTH(value)), 0) + 64 * COUNT(*) FROM ops",
        "SELECT COALESCE(SUM(LENGTH(payload)), 0) + 64 * COUNT(*) FROM archive_obs",
        "SELECT COALESCE(SUM(LENGTH(value)), 0) + 64 * COUNT(*) FROM profiles",
        "SELECT 64 * COUNT(*) FROM presence",
    ] {
        let value: i64 = store.connection().query_row(sql, [], |row| row.get(0))?;
        total += value.max(0) as u64;
    }
    Ok(total)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::store::event::{AccountId, Event, EventKind};
    use crate::store::ingest::ingest_batch_at;
    use crate::store::query;
    use crate::sync::clock::Hlc;

    fn account(byte: u8) -> AccountId {
        AccountId::new([byte; 16]).unwrap()
    }

    fn presence(account: AccountId, at_ms: u64, payload: &[u8]) -> Event {
        Event::new(
            account,
            "dev-a",
            EventKind::PresenceOnline,
            "presence:7",
            None,
            payload.to_vec(),
            Hlc::new(at_ms, 0),
        )
    }

    /// `count` presence events one millisecond apart, half of them "old".
    fn seed(store: &mut Store, account: AccountId, count: u64, base_ms: u64) {
        let events: Vec<Event> = (0..count)
            .map(|index| presence(account, base_ms + index, &(index as i64).to_be_bytes()))
            .collect();
        let outcome = ingest_batch_at(store, &account, &events, 0).unwrap();
        assert_eq!(outcome.applied, count);
    }

    #[test]
    fn purge_by_age_removes_old_rows_only() {
        let mut store = Store::open_in_memory().unwrap();
        let account = account(1);
        seed(&mut store, account, 5, 1_000);
        // `applied_at_ms` is 0 for every row from `seed`; make ages explicit.
        store
            .connection()
            .execute_batch(
                "UPDATE ops SET applied_at_ms = hlc_physical;
                 UPDATE presence SET observed_ms = hlc_physical;",
            )
            .unwrap();

        let report = purge_by_age(&mut store, 1_003).unwrap();
        assert_eq!(report.ops, 3, "rows at 1000, 1001, 1002 are older");
        assert_eq!(report.presence, 3);
        assert_eq!(query::op_count(&store, &account).unwrap(), 2);
        assert_eq!(
            query::presence_history(&store, &account, 7, 10)
                .unwrap()
                .len(),
            2
        );
        assert!(estimated_bytes(&store).unwrap() > 0);
    }

    #[test]
    fn purge_by_age_keeps_the_boundary_row() {
        let mut store = Store::open_in_memory().unwrap();
        let account = account(2);
        seed(&mut store, account, 3, 1_000);
        store
            .connection()
            .execute_batch("UPDATE ops SET applied_at_ms = hlc_physical;")
            .unwrap();
        // Strictly older than the cutoff are deleted; equal stays.
        let report = purge_by_age(&mut store, 1_001).unwrap();
        assert_eq!(report.ops, 1);
        assert_eq!(query::op_count(&store, &account).unwrap(), 2);
    }

    #[test]
    fn purge_by_age_is_idempotent() {
        let mut store = Store::open_in_memory().unwrap();
        let account = account(3);
        seed(&mut store, account, 4, 1_000);
        store
            .connection()
            .execute_batch("UPDATE ops SET applied_at_ms = hlc_physical;")
            .unwrap();
        let first = purge_by_age(&mut store, 1_002).unwrap();
        let second = purge_by_age(&mut store, 1_002).unwrap();
        assert_eq!(first.ops, 2);
        assert!(second.is_empty());
    }

    #[test]
    fn purge_by_age_touches_every_account() {
        let mut store = Store::open_in_memory().unwrap();
        let a = account(4);
        let b = account(5);
        seed(&mut store, a, 2, 1_000);
        seed(&mut store, b, 2, 1_000);
        store
            .connection()
            .execute_batch(
                "UPDATE ops SET applied_at_ms = hlc_physical;
                 UPDATE presence SET observed_ms = hlc_physical;",
            )
            .unwrap();
        let report = purge_by_age(&mut store, 5_000).unwrap();
        assert_eq!(report.ops, 4);
        assert_eq!(query::op_count(&store, &a).unwrap(), 0);
        assert_eq!(query::op_count(&store, &b).unwrap(), 0);
        // The account rows survive: retention is not account deletion.
        assert_eq!(query::accounts(&store).unwrap().len(), 2);
    }

    #[test]
    fn per_account_cap_keeps_the_newest_rows_and_projection_rows_together() {
        let mut store = Store::open_in_memory().unwrap();
        let a = account(6);
        let b = account(7);
        seed(&mut store, a, 5, 1_000);
        seed(&mut store, b, 2, 2_000);

        let report = purge_by_size(&mut store, Some(2), None).unwrap();
        assert_eq!(report.ops, 3, "account A had 3 rows above the cap, B none");
        assert_eq!(report.presence, 3, "projection rows go with their ops");
        assert_eq!(query::op_count(&store, &a).unwrap(), 2);
        assert_eq!(query::op_count(&store, &b).unwrap(), 2);

        // The surviving rows are the newest ones.
        let ops = query::ops_since(&store, &a, 0, 10).unwrap();
        assert_eq!(
            ops.iter().map(|op| op.op_seq).collect::<Vec<_>>(),
            vec![4, 5]
        );
        let presence = query::presence_history(&store, &a, 7, 10).unwrap();
        assert_eq!(presence.len(), 2);
        assert_eq!(presence[1].detail_ms, Some(3));
    }

    #[test]
    fn per_account_cap_is_a_no_op_when_under_the_limit() {
        let mut store = Store::open_in_memory().unwrap();
        let a = account(8);
        seed(&mut store, a, 2, 1_000);
        let report = purge_by_size(&mut store, Some(10), None).unwrap();
        assert!(report.is_empty());
        assert_eq!(query::op_count(&store, &a).unwrap(), 2);
    }

    #[test]
    fn byte_budget_trims_from_the_oldest_end() {
        let mut store = Store::open_in_memory().unwrap();
        let a = account(9);
        // 20 rows, each 8 payload bytes + 64 overhead from the HLC value.
        seed(&mut store, a, 20, 1_000);
        store
            .connection()
            .execute_batch("UPDATE ops SET applied_at_ms = hlc_physical;")
            .unwrap();
        let before = estimated_bytes(&store).unwrap();
        assert!(before > 0);

        let report = purge_by_size(&mut store, None, Some(before / 2)).unwrap();
        assert!(report.ops > 0, "something must be deleted");
        let after = estimated_bytes(&store).unwrap();
        assert!(after < before, "estimated size must go down");
        // Newest rows survived; the highest op_seq is still there.
        let ops = query::ops_since(&store, &a, 0, 100).unwrap();
        assert_eq!(ops.last().map(|op| op.op_seq), Some(20));
        assert!(ops.len() < 20);
    }

    #[test]
    fn byte_budget_that_is_already_met_deletes_nothing() {
        let mut store = Store::open_in_memory().unwrap();
        let a = account(10);
        seed(&mut store, a, 3, 1_000);
        let report = purge_by_size(&mut store, None, Some(u64::MAX)).unwrap();
        assert!(report.is_empty());
    }

    #[test]
    fn policy_applies_age_then_size() {
        let mut store = Store::open_in_memory().unwrap();
        let a = account(11);
        seed(&mut store, a, 10, 1_000);
        store
            .connection()
            .execute_batch(
                "UPDATE ops SET applied_at_ms = hlc_physical;
                 UPDATE presence SET observed_ms = hlc_physical;",
            )
            .unwrap();
        let policy = RetentionPolicy {
            max_age_ms: Some(5),
            max_ops_per_account: Some(2),
            max_bytes: None,
        };
        // now = 1009, cutoff = 1004: rows 1000..1003 age out, then the cap keeps 2.
        let report = policy.apply(&mut store, 1_009).unwrap();
        assert_eq!(report.ops, 8);
        assert_eq!(query::op_count(&store, &a).unwrap(), 2);
    }

    #[test]
    fn default_policy_keeps_ninety_days() {
        let policy = RetentionPolicy::default_for_vps();
        assert_eq!(policy.max_age_ms, Some(90 * 24 * 60 * 60 * 1_000));
        assert_eq!(policy.max_ops_per_account, None);
        assert_eq!(policy.max_bytes, None);

        let mut store = Store::open_in_memory().unwrap();
        let a = account(12);
        seed(&mut store, a, 2, 1_000);
        store
            .connection()
            .execute_batch("UPDATE ops SET applied_at_ms = hlc_physical;")
            .unwrap();
        // "Now" is far in the future, so everything ages out.
        let report = policy
            .apply(&mut store, 10 * 90 * 24 * 60 * 60 * 1_000)
            .unwrap();
        assert_eq!(report.ops, 2);
    }

    #[test]
    fn retention_never_rewrites_the_monotone_counters() {
        let mut store = Store::open_in_memory().unwrap();
        let a = account(13);
        seed(&mut store, a, 5, 1_000);
        let before = query::entity_state(&store, &a, "presence:7")
            .unwrap()
            .unwrap();
        let stamp_before = query::account_stamp(&store, &a).unwrap();
        purge_by_size(&mut store, Some(1), None).unwrap();
        let after = query::entity_state(&store, &a, "presence:7")
            .unwrap()
            .unwrap();
        let stamp_after = query::account_stamp(&store, &a).unwrap();
        assert_eq!(
            before.revision_count, after.revision_count,
            "revision_count is a monotone aggregate, not a live count"
        );
        assert_eq!(
            stamp_before.high_water, stamp_after.high_water,
            "high_water must not move: clients hold cursors against it"
        );
        assert_eq!(stamp_after.op_count, 1);
    }

    #[test]
    fn prune_upto_removes_history_before_the_cursor_and_keeps_high_water() {
        let mut store = Store::open_in_memory().unwrap();
        let a = account(14);
        seed(&mut store, a, 5, 1_000);
        let report = prune_upto(&mut store, &a, 3).unwrap();
        assert_eq!(report.ops, 3);
        assert_eq!(report.presence, 3, "projection rows go with their ops");
        assert_eq!(query::op_count(&store, &a).unwrap(), 2);
        assert_eq!(
            query::account_stamp(&store, &a).unwrap().high_water,
            5,
            "pruning must not rewind the cursor clients hold"
        );
        // Only the rows above the cursor survive.
        let ops = query::ops_since(&store, &a, 0, 10).unwrap();
        assert_eq!(
            ops.iter().map(|op| op.op_seq).collect::<Vec<_>>(),
            vec![4, 5]
        );
    }

    #[test]
    fn prune_beyond_high_water_is_a_conflict_not_a_delete() {
        let mut store = Store::open_in_memory().unwrap();
        let a = account(15);
        seed(&mut store, a, 3, 1_000);
        let error = prune_upto(&mut store, &a, 99).unwrap_err();
        assert!(matches!(error, crate::error::Error::Conflict(_)), "{error}");
        assert_eq!(
            query::op_count(&store, &a).unwrap(),
            3,
            "a refused prune must delete nothing"
        );
    }

    #[test]
    fn prune_records_the_floor_so_a_stale_cursor_can_be_told() {
        let mut store = Store::open_in_memory().unwrap();
        let a = account(16);
        seed(&mut store, a, 4, 1_000);
        assert_eq!(pruned_upto(&store, &a).unwrap(), None);
        prune_upto(&mut store, &a, 2).unwrap();
        assert_eq!(pruned_upto(&store, &a).unwrap(), Some(2));
        // A second, deeper prune moves the floor forward, never back.
        prune_upto(&mut store, &a, 3).unwrap();
        assert_eq!(pruned_upto(&store, &a).unwrap(), Some(3));
        // Another account is unaffected: the floor is per account.
        let b = account(17);
        assert_eq!(pruned_upto(&store, &b).unwrap(), None);
    }

    #[test]
    fn prune_is_idempotent() {
        let mut store = Store::open_in_memory().unwrap();
        let a = account(18);
        seed(&mut store, a, 3, 1_000);
        let first = prune_upto(&mut store, &a, 2).unwrap();
        let second = prune_upto(&mut store, &a, 2).unwrap();
        assert_eq!(first.ops, 2);
        assert!(second.is_empty());
        assert_eq!(query::op_count(&store, &a).unwrap(), 1);
    }

    #[test]
    fn purge_report_serialises() {
        let report = PurgeReport {
            ops: 1,
            presence: 2,
            profiles: 3,
            archive: 4,
        };
        assert_eq!(report.total(), 10);
        assert_eq!(
            report.to_json(),
            "{\"ops\":1,\"presence\":2,\"profiles\":3,\"archive\":4,\"total\":10}"
        );
        assert!(!report.is_empty());
        assert!(PurgeReport::default().is_empty());
    }
}
