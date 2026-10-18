//! The read cursor of the server-side DB ingest.
//!
//! # What it is not
//!
//! Not `device_clock` (that bounds a client device's HLC) and not
//! `accounts.high_water` (that is the last `op_seq` handed out for the account).
//! Both move for reasons that have nothing to do with the user's PostgreSQL/MySQL,
//! so overloading either would make one counter jump when the other moves and would
//! lose the only value the ingest actually needs: **which row of which table was
//! already published**.
//!
//! ```text
//! db_cursor(account_id, source, last_key, updated_at)
//!                       ^ the table the cursor belongs to
//! ```
//!
//! `source` is part of the key because two sync tables have incomparable key
//! spaces: `messages.id = 4127` says nothing about `online_events.id = 12`.
//!
//! # Monotonicity is enforced in SQL, not only in Rust
//!
//! [`advance`] and [`advance_in_tx`] never move `last_key` backwards:
//!
//! ```sql
//! last_key = CASE WHEN excluded.last_key > db_cursor.last_key
//!                 THEN excluded.last_key ELSE db_cursor.last_key END
//! ```
//!
//! A caller that retries an older batch — which is exactly what a retry after a
//! partial failure looks like — therefore cannot rewind the ingest and re-publish
//! history. `RETURNING last_key` hands back the value that was *stored*, so the
//! health report shows the cursor the store actually has, not the one the caller
//! asked for.
//!
//! # Why the cursor is written in the same transaction as the events
//!
//! [`crate::db::ingest`] calls [`advance_in_tx`] from inside the transaction that
//! inserted the `ops` rows. That is what makes "events published, cursor not
//! advanced" (a duplicate on the next pass) and "cursor advanced, events lost" (a
//! hole in the journal) impossible states rather than unlikely ones. It is the C4
//! rule of `docs/architecture.md` applied to the other direction of the same
//! problem.

use rusqlite::Connection;

use crate::error::{Error, Result};
use crate::store::conn::{Store, Tx};
use crate::store::event::AccountId;

/// Longest accepted `source`. A table name, not free text: this is bounded so a
/// caller cannot turn the cursor table into a key-value store.
pub const MAX_SOURCE_LEN: usize = 128;

/// One stored cursor row.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DbCursorRow {
    pub account_id: AccountId,
    pub source: String,
    /// Last key (row id / monotone column) that was published.
    pub last_key: i64,
    pub updated_at: i64,
}

impl DbCursorRow {
    /// The table the cursor belongs to (§ 5.8 `db.status` publishes it as
    /// `cursor.table`).
    pub fn source(&self) -> &str {
        &self.source
    }
}

/// Reject a source that cannot be a table name. Called before any SQL, so the check
/// is about *shape*, not about the allowlist — the allowlist is
/// [`crate::db::allowlist`]'s job.
pub fn check_source(source: &str) -> Result<()> {
    if source.is_empty() || source.len() > MAX_SOURCE_LEN {
        return Err(Error::Invalid(format!(
            "cursor source must be 1..={MAX_SOURCE_LEN} characters"
        )));
    }
    if !source
        .bytes()
        .all(|byte| byte.is_ascii_alphanumeric() || matches!(byte, b'_' | b'.' | b'-'))
    {
        return Err(Error::Invalid(format!(
            "cursor source {source:?} must be a table name: [A-Za-z0-9_.-] only"
        )));
    }
    Ok(())
}

/// Read the cursor for `(account, source)`, or `None` when nothing was read yet.
pub fn read(store: &Store, account: &AccountId, source: &str) -> Result<Option<DbCursorRow>> {
    read_conn(store.connection(), account, source)
}

/// [`read`] from inside an open transaction.
pub fn read_in_tx(tx: &Tx<'_>, account: &AccountId, source: &str) -> Result<Option<DbCursorRow>> {
    read_conn(tx.connection(), account, source)
}

fn read_conn(conn: &Connection, account: &AccountId, source: &str) -> Result<Option<DbCursorRow>> {
    check_source(source)?;
    use rusqlite::OptionalExtension;
    let row: Option<(Vec<u8>, String, i64, i64)> = conn
        .query_row(
            "SELECT account_id, source, last_key, updated_at
               FROM db_cursor
              WHERE account_id = ?1 AND source = ?2",
            rusqlite::params![account.as_blob(), source],
            |row| Ok((row.get(0)?, row.get(1)?, row.get(2)?, row.get(3)?)),
        )
        .optional()?;
    match row {
        None => Ok(None),
        Some((blob, source, last_key, updated_at)) => Ok(Some(DbCursorRow {
            account_id: AccountId::from_slice(&blob)?,
            source,
            last_key,
            updated_at,
        })),
    }
}

/// Move the cursor forward to at least `last_key`, in its own transaction.
pub fn advance(
    store: &mut Store,
    account: &AccountId,
    source: &str,
    last_key: i64,
    now_ms: i64,
) -> Result<DbCursorRow> {
    store.transaction(|tx| advance_in_tx(tx, account, source, last_key, now_ms))
}

/// Move the cursor forward inside the caller's transaction.
///
/// This is the function the ingest uses: the cursor and the `ops` rows it accounts
/// for commit together. Returns the value the store ended up with, which may be
/// larger than `last_key` (monotonicity, see the module docs).
pub fn advance_in_tx(
    tx: &Tx<'_>,
    account: &AccountId,
    source: &str,
    last_key: i64,
    now_ms: i64,
) -> Result<DbCursorRow> {
    check_source(source)?;
    // The row has a foreign key to `accounts`, and a batch whose events were all
    // rejected never touches it — so the account is registered here, on the path
    // that is about to write the cursor, rather than relying on the caller.
    tx.ensure_account(account, now_ms)?;
    let stored: i64 = tx.connection().query_row(
        "INSERT INTO db_cursor (account_id, source, last_key, updated_at)
              VALUES (?1, ?2, ?3, ?4)
         ON CONFLICT (account_id, source) DO UPDATE SET
              last_key   = CASE WHEN excluded.last_key > db_cursor.last_key
                                THEN excluded.last_key ELSE db_cursor.last_key END,
              updated_at = excluded.updated_at
         RETURNING last_key",
        rusqlite::params![account.as_blob(), source, last_key, now_ms],
        |row| row.get(0),
    )?;
    Ok(DbCursorRow {
        account_id: *account,
        source: source.to_string(),
        last_key: stored,
        updated_at: now_ms,
    })
}

/// Drop the cursor for one `(account, source)`.
///
/// Only used when an operator deliberately republishes a table from the beginning;
/// nothing on the ingest path calls it, because "forget the cursor" means
/// "publish the same rows again".
pub fn clear(store: &mut Store, account: &AccountId, source: &str) -> Result<bool> {
    check_source(source)?;
    let removed = store.connection().execute(
        "DELETE FROM db_cursor WHERE account_id = ?1 AND source = ?2",
        rusqlite::params![account.as_blob(), source],
    )?;
    Ok(removed > 0)
}

/// Every cursor of one account, ordered by source. Diagnostic helper for
/// `self-test` and for the operator-facing report.
pub fn list(store: &Store, account: &AccountId) -> Result<Vec<DbCursorRow>> {
    let mut statement = store.connection().prepare(
        "SELECT account_id, source, last_key, updated_at
           FROM db_cursor WHERE account_id = ?1 ORDER BY source ASC",
    )?;
    let mut rows = statement.query([account.as_blob()])?;
    let mut out = Vec::new();
    while let Some(row) = rows.next()? {
        let blob: Vec<u8> = row.get(0)?;
        out.push(DbCursorRow {
            account_id: AccountId::from_slice(&blob)?,
            source: row.get(1)?,
            last_key: row.get(2)?,
            updated_at: row.get(3)?,
        });
    }
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn account(byte: u8) -> AccountId {
        AccountId::new([byte; 16]).unwrap()
    }

    fn store() -> Store {
        Store::open_in_memory().unwrap()
    }

    #[test]
    fn an_absent_cursor_reads_as_none() {
        let store = store();
        assert_eq!(read(&store, &account(1), "sync_outbox").unwrap(), None);
    }

    #[test]
    fn the_cursor_is_created_moved_and_read_back() {
        let mut store = store();
        let account = account(1);
        let row = advance(&mut store, &account, "sync_outbox", 41, 1_000).unwrap();
        assert_eq!(row.last_key, 41);
        assert_eq!(row.updated_at, 1_000);
        assert_eq!(row.account_id, account);

        let read_back = read(&store, &account, "sync_outbox").unwrap().unwrap();
        assert_eq!(read_back, row);

        let moved = advance(&mut store, &account, "sync_outbox", 99, 2_000).unwrap();
        assert_eq!(moved.last_key, 99);
        assert_eq!(moved.updated_at, 2_000);
    }

    #[test]
    fn the_cursor_never_moves_backwards() {
        // This is the restart guarantee: `last_key` is monotone in SQL, so a retry
        // of an older batch cannot rewind the ingest.
        let mut store = store();
        let account = account(2);
        advance(&mut store, &account, "sync_outbox", 500, 1_000).unwrap();
        let rewound = advance(&mut store, &account, "sync_outbox", 10, 2_000).unwrap();
        assert_eq!(rewound.last_key, 500, "an older key must not win");
        // The timestamp still follows the last call: it describes when the row was
        // touched, not what the cursor is.
        assert_eq!(rewound.updated_at, 2_000);
        assert_eq!(
            read(&store, &account, "sync_outbox")
                .unwrap()
                .unwrap()
                .last_key,
            500
        );
    }

    #[test]
    fn cursors_are_isolated_per_source_and_per_account() {
        let mut store = store();
        let first = account(3);
        let second = account(4);
        advance(&mut store, &first, "sync_outbox", 7, 1_000).unwrap();
        advance(&mut store, &second, "sync_outbox", 8, 1_000).unwrap();
        advance(&mut store, &first, "online_events", 9, 1_000).unwrap();

        assert_eq!(
            read(&store, &first, "sync_outbox")
                .unwrap()
                .unwrap()
                .last_key,
            7
        );
        assert_eq!(
            read(&store, &second, "sync_outbox")
                .unwrap()
                .unwrap()
                .last_key,
            8
        );
        assert_eq!(
            read(&store, &first, "online_events")
                .unwrap()
                .unwrap()
                .last_key,
            9
        );
        assert_eq!(read(&store, &second, "online_events").unwrap(), None);
        assert_eq!(list(&store, &first).unwrap().len(), 2);
        // Per-account isolation, asserted rather than assumed (C3).
        assert!(list(&store, &account(5)).unwrap().is_empty());
    }

    #[test]
    fn the_cursor_registers_the_account_it_belongs_to() {
        let mut store = store();
        let account = account(6);
        // No other write happened for this account, so the cursor itself must
        // satisfy the foreign key.
        advance(&mut store, &account, "sync_outbox", 1, 1_000).unwrap();
        let known = crate::store::query::accounts(&store).unwrap();
        assert!(known.iter().any(|info| info.account_id == account));
    }

    #[test]
    fn a_bad_source_is_refused_before_any_sql() {
        let mut store = store();
        let account = account(7);
        for bad in ["", "with space", "semi;colon", "quote'name"] {
            let error = advance(&mut store, &account, bad, 1, 1_000).unwrap_err();
            assert!(
                error.to_string().contains("cursor source"),
                "{bad:?} must be refused, got {error}"
            );
        }
        let long = "a".repeat(MAX_SOURCE_LEN + 1);
        assert!(advance(&mut store, &account, &long, 1, 1_000).is_err());
        // A dotted schema-qualified name is legal.
        advance(&mut store, &account, "public.sync_outbox", 1, 1_000).unwrap();
    }

    #[test]
    fn clearing_removes_only_that_cursor() {
        let mut store = store();
        let account = account(8);
        advance(&mut store, &account, "sync_outbox", 3, 1_000).unwrap();
        advance(&mut store, &account, "online_events", 4, 1_000).unwrap();
        assert!(clear(&mut store, &account, "sync_outbox").unwrap());
        assert_eq!(read(&store, &account, "sync_outbox").unwrap(), None);
        assert!(read(&store, &account, "online_events").unwrap().is_some());
        assert!(!clear(&mut store, &account, "sync_outbox").unwrap());
    }

    #[test]
    fn a_cursor_row_reports_its_source_for_db_status() {
        let row = DbCursorRow {
            account_id: account(9),
            source: "sync_outbox".to_string(),
            last_key: 4127,
            updated_at: 1_000,
        };
        assert_eq!(row.source(), "sync_outbox");
        assert_eq!(row.last_key, 4127);
    }
}
