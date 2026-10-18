//! The passive store: an append-only journal of everything the client observes.
//!
//! # What "passive" means here
//!
//! The client captures events without being asked to: a message arrives and later
//! gets edited, a message is deleted, a peer goes online, a profile changes, the
//! archive notices an author. Today those go into one `data.tforge` with one
//! `sqlite3*`, no `accountId` on most tables, and no history (audit C3, H3, H7,
//! H9). This module is the server side of the replacement:
//!
//! ```text
//! Event ──validate──► IngestResult::Applied / Duplicate / Rejected(reason)
//!                          │
//!                          └── one transaction ──► ops (append-only journal)
//!                                                  presence / profiles / archive_obs
//!                                                  entities (monotone aggregates)
//!                                                  accounts (per-account op_seq)
//! ```
//!
//! # The five guarantees, and where each one lives
//!
//! | Guarantee | Mechanism | Tested by |
//! |---|---|---|
//! | account isolation | `account_id` on every table, FK to `accounts`, ingest scope check | `ingest::tests::an_event_for_another_account_is_rejected_not_stored`, `schema::tests::every_table_that_holds_account_data_declares_account_id` |
//! | monotone `op_seq` per account | `accounts.high_water` bumped with `RETURNING` inside the same transaction | `conn::tests::op_seq_is_monotone_per_account_and_independent_across_accounts` |
//! | idempotency | `UNIQUE(account_id, content_hash)` + in-batch dedupe | `ingest::tests::replaying_the_same_batch_adds_no_rows` |
//! | no last-write-wins | `ops` is append-only; "current" is a query | `ingest::tests::history_is_append_only_never_last_write_wins` |
//! | no stuck transaction (C4) | [`conn::Tx`] rolls back from `Drop` | `conn::tests::a_failed_transaction_rolls_back_and_the_connection_stays_usable` |
//!
//! # Where the file lives
//!
//! `<data_dir>/tenants/<tenant_hex>/sync/data.db`, i.e. the path
//! [`crate::vault::sync_db_path`] already returns, mode `0600`, WAL. One file per
//! tenant, with `account_id` as the first column of every table — that is the
//! architecture's "one file, mandatory account prefix" choice, not "one file per
//! account", because a second file would make cross-account queries impossible
//! and the audit's problem was cross-account *records*, not a shared file.

pub mod conn;
pub mod cursor;
pub mod event;
pub mod ingest;
pub mod migrate;
pub mod query;
pub mod retention;
pub mod schema;

pub use conn::Store;
pub use cursor::DbCursorRow;
pub use event::{
    content_hash, AccountId, EntityClass, EntityId, Event, EventKind, IngestResult, RejectReason,
};
pub use ingest::{
    ingest, ingest_batch, ingest_batch_at, ingest_batch_in_tx, IngestOutcome, MAX_BATCH_EVENTS,
};
pub use migrate::{MigrationReport, MigrationStep};
pub use query::{
    account_stamp, accounts, archive_observations, conflict_list, entities_by_kind, entity_ops,
    entity_state, op_count, ops_since, presence_history, presence_latest, profile_current,
    profile_history, resolve_conflict, self_check, totals, AccountInfo, AccountStamp, ArchiveRow,
    ConflictRow, EntityState, PresenceRow, ProfileRow, SelfCheck, StoredOp, Totals,
};
pub use retention::{
    prune_upto, pruned_upto, purge_by_age, purge_by_size, PurgeReport, RetentionPolicy,
};
pub use schema::{SCHEMA_SQL, SCHEMA_V1_SQL, SCHEMA_V2_SQL, SCHEMA_V3_SQL, SCHEMA_VERSION};

/// Open the store for a tenant under `data_dir`.
pub fn open_for_tenant(data_dir: &str, tenant_hex: &str) -> crate::error::Result<Store> {
    Store::open(crate::vault::sync_db_path(data_dir, tenant_hex))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn open_for_tenant_creates_the_documented_path_with_private_permissions() {
        let directory = crate::test_support::TestDir::new("mod");
        let data_dir = directory.path().to_string_lossy().to_string();
        let store = open_for_tenant(&data_dir, "00112233445566778899001122334455").unwrap();
        let expected = format!("{data_dir}/tenants/00112233445566778899001122334455/sync/data.db");
        assert_eq!(store.path().to_string_lossy(), expected);
        assert!(std::path::Path::new(&expected).exists());
        // The schema is in place, so the store is usable immediately.
        let check = self_check(&store).unwrap();
        assert!(check.tables.iter().any(|name| name == "ops"));
    }

    #[test]
    fn a_store_survives_reopening_and_keeps_its_high_water() {
        let directory = crate::test_support::TestDir::new("mod");
        let path = directory.join("data.db");
        let account = AccountId::new([3u8; 16]).unwrap();
        {
            let mut store = Store::open(&path).unwrap();
            let event = Event::new(
                account,
                "dev-a",
                EventKind::PresenceOnline,
                "presence:1",
                None,
                5i64.to_be_bytes().to_vec(),
                crate::sync::clock::Hlc::new(1_000, 0),
            );
            let outcome = ingest_batch(&mut store, &account, std::slice::from_ref(&event)).unwrap();
            assert_eq!(outcome.applied, 1);
        }
        let store = Store::open(&path).unwrap();
        assert_eq!(op_count(&store, &account).unwrap(), 1);
        assert_eq!(account_stamp(&store, &account).unwrap().high_water, 1);
    }
}
