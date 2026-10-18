//! Schema migrations.
//!
//! # Why a step list and not `ALTER TABLE` scattered in `Store::open`
//!
//! A store file outlives the binary that wrote it: the client installs a new agent
//! over the old one and the `data.db` from last month is still there. Two failure
//! modes matter, and both are cheap to prevent:
//!
//! * **Half-applied migration.** If the `schema_version` bump is a separate
//!   statement from the `ALTER TABLE`, a crash in between leaves a file that claims
//!   to be v1 but already has the v2 column (or the reverse). Here the DDL and the
//!   version bump are in **one** transaction, and every step checks for the object
//!   it is about to create, so re-running a step after a crash is a no-op.
//! * **Guessing.** A file written by a *newer* agent is refused
//!   ([`Error::Storage`]), never "opened anyway": a newer step may have added a
//!   `NOT NULL` column this build cannot fill, and silently operating on it would
//!   corrupt exactly the data the user is trying to keep.
//!
//! # Idempotence is asserted by the columns, not by the version number
//!
//! The version row is the *fast path*, not the source of truth. If the row says v2
//! but a column is missing (an interrupted `ALTER` on a filesystem that lost the
//! journal), the migration still runs and repairs it. That is why the check is
//! `PRAGMA table_info`, not `if version == 2 { return }`.

use rusqlite::{Connection, OptionalExtension};

use crate::error::{Error, Result};
use crate::store::schema::{SCHEMA_V2_SQL, SCHEMA_V3_SQL, SCHEMA_VERSION, V2_ADDED_COLUMNS};

/// One upgrade step. `from`/`to` are the versions on either side; the list must be
/// a contiguous chain starting at 1 and ending at [`SCHEMA_VERSION`], which
/// [`tests::the_step_list_reaches_the_current_version`] enforces.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct MigrationStep {
    pub from: u32,
    pub to: u32,
    pub name: &'static str,
}

/// Every migration this build knows. Append-only: a released step is never edited,
/// because a store somewhere already ran it.
pub const STEPS: [MigrationStep; 2] = [
    MigrationStep {
        from: 1,
        to: 2,
        name: "conflicts-and-conflict-counter",
    },
    // The server-side DB ingest needs its own read position; `device_clock` and
    // `accounts.high_water` describe devices and the journal, not the user's
    // PostgreSQL/MySQL (`docs/db-ingest.md` § 4).
    MigrationStep {
        from: 2,
        to: 3,
        name: "db-cursor",
    },
];

/// What [`run`] did.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct MigrationReport {
    /// Version found in `schema_meta`.
    pub from: u32,
    /// Version after the run.
    pub to: u32,
    /// True when DDL or the version row was actually written.
    pub upgraded: bool,
    /// Names of the steps that were needed, in order.
    pub steps: Vec<&'static str>,
}

impl MigrationReport {
    pub fn is_noop(&self) -> bool {
        !self.upgraded
    }
}

/// Bring `conn` up to [`SCHEMA_VERSION`], idempotently.
///
/// Called from [`crate::store::conn::Store::finish_open`] on **every** open, so a
/// normal start is one `PRAGMA table_info` per added column and one no-op
/// `CREATE TABLE IF NOT EXISTS`.
pub fn run(conn: &mut Connection) -> Result<MigrationReport> {
    let from = read_version(conn)?;
    if from > SCHEMA_VERSION {
        return Err(Error::Storage(format!(
            "store schema_version {from} is newer than this agent ({SCHEMA_VERSION}); upgrade the agent"
        )));
    }

    // Validate that a path exists before touching anything, so an impossible
    // downgrade/upgrade reports "no path" instead of a half-run.
    let mut steps = Vec::new();
    let mut cursor = from;
    while cursor < SCHEMA_VERSION {
        let step = STEPS
            .iter()
            .find(|step| step.from == cursor)
            .ok_or_else(|| {
                Error::Storage(format!(
                    "no migration path from schema {cursor} to {SCHEMA_VERSION}"
                ))
            })?;
        steps.push(step.name);
        cursor = step.to;
    }

    if steps.is_empty() && schema_is_current(conn)? {
        return Ok(MigrationReport {
            from,
            to: from,
            upgraded: false,
            steps,
        });
    }

    let transaction = conn.transaction()?;
    ensure_current(&transaction)?;
    transaction.execute(
        "UPDATE schema_meta SET value = ?1 WHERE key = 'schema_version'",
        [SCHEMA_VERSION.to_string()],
    )?;
    transaction.commit()?;

    Ok(MigrationReport {
        from,
        to: SCHEMA_VERSION,
        upgraded: true,
        steps,
    })
}

/// The version recorded in `schema_meta`, or an error when the row is absent or not
/// a number. Absent is an error rather than "assume v1": a file without the marker
/// is not a file this agent wrote, and guessing its layout is how data gets lost.
pub fn read_version(conn: &Connection) -> Result<u32> {
    let stored: Option<String> = conn
        .query_row(
            "SELECT value FROM schema_meta WHERE key = 'schema_version'",
            [],
            |row| row.get(0),
        )
        .optional()?;
    let Some(stored) = stored else {
        return Err(Error::Storage(
            "store has no schema_version row; refusing to guess the layout".to_string(),
        ));
    };
    stored
        .parse::<u32>()
        .map_err(|_| Error::Storage(format!("store schema_version {stored:?} is not a number")))
}

/// True when every current object exists. Used only to skip the write transaction
/// on the happy path.
fn schema_is_current(conn: &Connection) -> Result<bool> {
    for (table, column) in V2_ADDED_COLUMNS {
        if !column_exists(conn, table, column)? {
            return Ok(false);
        }
    }
    if !table_exists(conn, "conflicts")? {
        return Ok(false);
    }
    table_exists(conn, "db_cursor")
}

/// Apply every additive change. Safe to call when they are already applied.
fn ensure_current(conn: &Connection) -> Result<()> {
    for (table, column) in V2_ADDED_COLUMNS {
        if !column_exists(conn, table, column)? {
            // `NOT NULL DEFAULT 0` is required for `ALTER TABLE ADD COLUMN` to be
            // valid on a non-empty table, and 0 is the correct value: no conflicts
            // have been recorded yet.
            conn.execute_batch(&format!(
                "ALTER TABLE {table} ADD COLUMN {column} INTEGER NOT NULL DEFAULT 0"
            ))?;
        }
    }
    // Both batches are `IF NOT EXISTS`, so re-running them is the idempotent
    // "repair an interrupted step" path as well as the normal one.
    conn.execute_batch(SCHEMA_V2_SQL)?;
    conn.execute_batch(SCHEMA_V3_SQL)?;
    Ok(())
}

pub fn table_exists(conn: &Connection, table: &str) -> Result<bool> {
    let found: Option<i64> = conn
        .query_row(
            "SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ?1",
            [table],
            |row| row.get(0),
        )
        .optional()?;
    Ok(found.is_some())
}

pub fn column_exists(conn: &Connection, table: &str, column: &str) -> Result<bool> {
    // `PRAGMA table_info` takes no bound parameters, so the table name is
    // interpolated. It is a compile-time constant at every call site (see
    // `V2_ADDED_COLUMNS`), never wire input.
    let mut statement = conn.prepare(&format!("PRAGMA table_info({table})"))?;
    let mut rows = statement.query([])?;
    while let Some(row) = rows.next()? {
        let name: String = row.get(1)?;
        if name == column {
            return Ok(true);
        }
    }
    Ok(false)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::store::conn::Store;
    use crate::store::query::{self, op_count};
    use crate::store::schema::SCHEMA_V1_SQL;
    use crate::store::{AccountId, SCHEMA_VERSION as CURRENT};

    fn account() -> AccountId {
        AccountId::new([0x2a; 16]).unwrap()
    }

    /// A v1 database with real data, written by raw SQL — i.e. exactly what an older
    /// agent would have left behind.
    fn seed_v1(path: &std::path::Path) -> AccountId {
        let account = account();
        let conn = Connection::open(path).unwrap();
        conn.execute_batch(SCHEMA_V1_SQL).unwrap();
        conn.execute(
            "INSERT INTO accounts (account_id, created_at_ms, last_write_ms, high_water,
                                   device_count)
             VALUES (?1, 1000, 2000, 3, 2)",
            [account.as_blob()],
        )
        .unwrap();
        for (seq, hash_byte, entity, kind) in [
            (1u64, 1u8, "profile:900", 6i64),
            (2, 2, "profile:900", 6),
            (3, 3, "presence:900", 4),
        ] {
            conn.execute(
                "INSERT INTO ops (op_seq, account_id, kind, entity_id, field, value,
                                  hlc_physical, hlc_logical, device_id, content_hash,
                                  applied_at_ms)
                 VALUES (?1, ?2, ?3, ?4, 'username', ?5, ?6, 0, 'dev-a', ?7, 1500)",
                rusqlite::params![
                    seq as i64,
                    account.as_blob(),
                    kind,
                    entity,
                    vec![hash_byte; 4],
                    1_000 + seq as i64,
                    vec![hash_byte; 32],
                ],
            )
            .unwrap();
        }
        conn.execute(
            "INSERT INTO presence (account_id, op_seq, peer_id, status, detail_ms,
                                   observed_ms, hlc_physical, hlc_logical, device_id,
                                   content_hash)
             VALUES (?1, 3, 900, 4, 5000, 1003, 1003, 0, 'dev-a', ?2)",
            rusqlite::params![account.as_blob(), vec![3u8; 32]],
        )
        .unwrap();
        conn.execute(
            "INSERT INTO device_clock (account_id, device_id, last_hlc_physical,
                                       last_hlc_logical, last_seen_ms)
             VALUES (?1, 'dev-a', 1003, 0, 1500)",
            [account.as_blob()],
        )
        .unwrap();
        // The v1 fixture must really be v1.
        let version: String = conn
            .query_row(
                "SELECT value FROM schema_meta WHERE key = 'schema_version'",
                [],
                |row| row.get(0),
            )
            .unwrap();
        assert_eq!(version, "1");
        account
    }

    #[test]
    fn a_v1_file_is_upgraded_with_its_data_intact() {
        let directory = crate::test_support::TestDir::new("migrate");
        let path = directory.join("data.db");
        let account = seed_v1(&path);

        let store = Store::open(&path).expect("the v1 file must open");
        let version: String = store
            .connection()
            .query_row(
                "SELECT value FROM schema_meta WHERE key = 'schema_version'",
                [],
                |row| row.get(0),
            )
            .unwrap();
        assert_eq!(version, CURRENT.to_string());

        // Data is intact, including the monotone counters that retention and cursors
        // depend on.
        assert_eq!(op_count(&store, &account).unwrap(), 3);
        assert_eq!(
            query::account_stamp(&store, &account).unwrap().high_water,
            3
        );
        assert_eq!(
            query::presence_history(&store, &account, 900, 10)
                .unwrap()
                .len(),
            1
        );
        let accounts = query::accounts(&store).unwrap();
        assert_eq!(accounts.len(), 1);
        assert_eq!(accounts[0].device_count, 2);

        // v2 objects exist.
        assert!(table_exists(store.connection(), "conflicts").unwrap());
        assert!(column_exists(store.connection(), "accounts", "conflict_high_water").unwrap());
        // The added column has a usable default on pre-existing rows.
        let counter: i64 = store
            .connection()
            .query_row(
                "SELECT conflict_high_water FROM accounts WHERE account_id = ?1",
                [account.as_blob()],
                |row| row.get(0),
            )
            .unwrap();
        assert_eq!(counter, 0);

        let check = query::self_check(&store).unwrap();
        assert!(
            check.ok(),
            "problems after migration: {:?}",
            check.problems()
        );
    }

    #[test]
    fn migrating_twice_changes_nothing() {
        let directory = crate::test_support::TestDir::new("migrate");
        let path = directory.join("data.db");
        let account = seed_v1(&path);

        {
            let store = Store::open(&path).unwrap();
            assert_eq!(op_count(&store, &account).unwrap(), 3);
        }
        let mut conn = Connection::open(&path).unwrap();
        let report = run(&mut conn).unwrap();
        assert!(report.is_noop(), "second run must be a no-op");
        assert_eq!(report.from, CURRENT);
        assert_eq!(report.to, CURRENT);
        // ... and the data still opens through the normal path.
        let store = Store::open(&path).unwrap();
        assert_eq!(op_count(&store, &account).unwrap(), 3);
    }

    #[test]
    fn a_fresh_store_reports_the_current_version_and_needs_no_steps() {
        let mut conn = Connection::open_in_memory().unwrap();
        conn.execute_batch(crate::store::schema::SCHEMA_SQL)
            .unwrap();
        conn.execute_batch(SCHEMA_V2_SQL).unwrap();
        conn.execute_batch(SCHEMA_V3_SQL).unwrap();
        let report = run(&mut conn).unwrap();
        assert_eq!(report.from, CURRENT);
        assert!(report.steps.is_empty());
        assert!(report.is_noop());
    }

    #[test]
    fn a_file_from_the_future_is_refused_not_guessed() {
        let directory = crate::test_support::TestDir::new("migrate");
        let path = directory.join("data.db");
        let _ = seed_v1(&path);
        {
            let conn = Connection::open(&path).unwrap();
            conn.execute(
                "UPDATE schema_meta SET value = '99' WHERE key = 'schema_version'",
                [],
            )
            .unwrap();
        }
        let error = Store::open(&path).unwrap_err();
        assert!(error.to_string().contains("newer"), "{error}");
    }

    #[test]
    fn a_missing_version_row_is_repaired_by_the_schema_batch() {
        // `SCHEMA_SQL` re-inserts the marker with `INSERT OR IGNORE`, so a deleted
        // row comes back on the next open instead of wedging the store. The
        // "refuse to guess" path in `read_version` is for a connection that never
        // ran the batch — asserted separately below.
        let directory = crate::test_support::TestDir::new("migrate");
        let path = directory.join("data.db");
        let account = seed_v1(&path);
        {
            let conn = Connection::open(&path).unwrap();
            conn.execute("DELETE FROM schema_meta WHERE key = 'schema_version'", [])
                .unwrap();
        }
        let store = Store::open(&path).expect("the marker is recreated");
        assert_eq!(
            crate::store::migrate::read_version(store.connection()).unwrap(),
            CURRENT
        );
        assert_eq!(op_count(&store, &account).unwrap(), 3);
    }

    #[test]
    fn a_connection_without_the_marker_refuses_to_guess() {
        let mut conn = Connection::open_in_memory().unwrap();
        conn.execute_batch("CREATE TABLE schema_meta (key TEXT PRIMARY KEY, value TEXT) STRICT")
            .unwrap();
        let error = run(&mut conn).unwrap_err();
        assert!(error.to_string().contains("schema_version"), "{error}");
    }

    #[test]
    fn an_interrupted_migration_is_repaired_by_the_next_open() {
        // Simulate "DDL applied, version row not bumped" — the exact state a crash
        // between two statements used to leave behind.
        let directory = crate::test_support::TestDir::new("migrate");
        let path = directory.join("data.db");
        let account = seed_v1(&path);
        {
            let conn = Connection::open(&path).unwrap();
            conn.execute_batch(SCHEMA_V2_SQL).unwrap();
            conn.execute_batch(
                "ALTER TABLE accounts ADD COLUMN conflict_high_water INTEGER NOT NULL DEFAULT 0",
            )
            .unwrap();
        }
        let store = Store::open(&path).unwrap();
        let version: String = store
            .connection()
            .query_row(
                "SELECT value FROM schema_meta WHERE key = 'schema_version'",
                [],
                |row| row.get(0),
            )
            .unwrap();
        assert_eq!(version, CURRENT.to_string(), "version must be repaired");
        assert_eq!(op_count(&store, &account).unwrap(), 3);
    }

    #[test]
    fn the_step_list_reaches_the_current_version() {
        let mut cursor = 1u32;
        for step in STEPS {
            assert_eq!(step.from, cursor, "the step list must be contiguous");
            assert_eq!(step.to, step.from + 1);
            assert!(!step.name.is_empty());
            cursor = step.to;
        }
        assert_eq!(cursor, SCHEMA_VERSION);
    }

    #[test]
    fn the_added_columns_are_declared_in_the_current_ddl() {
        for (table, column) in V2_ADDED_COLUMNS {
            let ddl = format!(
                "{} {} {}",
                crate::store::schema::SCHEMA_SQL,
                SCHEMA_V2_SQL,
                SCHEMA_V3_SQL
            );
            assert!(
                ddl.contains(column),
                "{table}.{column} is added by migration but absent from the DDL"
            );
        }
    }

    /// A **v2** file: the shape the previous agent left behind, which is what the
    /// v2 → v3 step has to handle. Built from the v1 fixture plus exactly the v2
    /// additions, with the version marker rolled back to 2.
    fn seed_v2(path: &std::path::Path) -> AccountId {
        let account = seed_v1(path);
        let conn = Connection::open(path).unwrap();
        conn.execute_batch(SCHEMA_V2_SQL).unwrap();
        conn.execute_batch(
            "ALTER TABLE accounts ADD COLUMN conflict_high_water INTEGER NOT NULL DEFAULT 0",
        )
        .unwrap();
        conn.execute(
            "UPDATE schema_meta SET value = '2' WHERE key = 'schema_version'",
            [],
        )
        .unwrap();
        assert!(!table_exists(&conn, "db_cursor").unwrap());
        account
    }

    #[test]
    fn a_v2_file_gains_the_db_cursor_and_keeps_its_data() {
        let directory = crate::test_support::TestDir::new("migrate");
        let path = directory.join("data.db");
        let account = seed_v2(&path);

        let store = Store::open(&path).expect("the v2 file must open");
        assert_eq!(
            read_version(store.connection()).unwrap(),
            CURRENT,
            "a v2 file must be bumped to the current version"
        );
        assert!(table_exists(store.connection(), "db_cursor").unwrap());
        // Nothing was rewritten: the journal, the counters and the v2 table are
        // exactly as they were.
        assert_eq!(op_count(&store, &account).unwrap(), 3);
        assert_eq!(
            query::account_stamp(&store, &account).unwrap().high_water,
            3
        );
        assert!(table_exists(store.connection(), "conflicts").unwrap());
        assert!(column_exists(store.connection(), "accounts", "conflict_high_water").unwrap());
        // ... and the new cursor is usable straight away.
        assert_eq!(
            crate::store::cursor::read(&store, &account, "sync_outbox").unwrap(),
            None
        );
    }

    #[test]
    fn the_db_cursor_step_is_idempotent() {
        let directory = crate::test_support::TestDir::new("migrate");
        let path = directory.join("data.db");
        let account = seed_v2(&path);
        {
            let store = Store::open(&path).unwrap();
            assert_eq!(op_count(&store, &account).unwrap(), 3);
        }
        let mut conn = Connection::open(&path).unwrap();
        let report = run(&mut conn).unwrap();
        assert!(report.is_noop(), "a second migration run must be a no-op");
        assert_eq!(report.steps.len(), 0);
        // A third open still sees a consistent file.
        let store = Store::open(&path).unwrap();
        assert_eq!(op_count(&store, &account).unwrap(), 3);
    }

    #[test]
    fn an_interrupted_v3_migration_is_repaired_by_the_next_open() {
        // "Table created, version not bumped": the DDL-idempotent half of the step.
        let directory = crate::test_support::TestDir::new("migrate");
        let path = directory.join("data.db");
        let account = seed_v2(&path);
        {
            let conn = Connection::open(&path).unwrap();
            conn.execute_batch(SCHEMA_V3_SQL).unwrap();
        }
        let store = Store::open(&path).unwrap();
        assert_eq!(read_version(store.connection()).unwrap(), CURRENT);
        assert_eq!(op_count(&store, &account).unwrap(), 3);
    }

    #[test]
    fn the_step_list_covers_db_cursor() {
        let step = STEPS
            .iter()
            .find(|step| step.to == 3)
            .expect("the v3 step must be declared");
        assert_eq!(step.from, 2);
        assert_eq!(step.name, "db-cursor");
        assert!(SCHEMA_V3_SQL.contains("db_cursor"));
    }
}
