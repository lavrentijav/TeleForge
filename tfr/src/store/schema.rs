//! DDL for the passive store, kept as one reviewable constant.
//!
//! # What this schema is fixing
//!
//! * **C3 (one DB for all accounts).** Every table starts with `account_id`, and
//!   every table that carries per-account data declares a foreign key to
//!   `accounts`. A write without a registered account is a `FOREIGN KEY`
//!   violation, not a row that later leaks into another account's view.
//! * **C4 (a transaction left open "mutes" the passive path).** Nothing here is
//!   written outside [`crate::store::conn::Tx`], which issues `ROLLBACK` from
//!   `Drop`; see `conn.rs` and its regression test.
//! * **H3 (retention that never runs, index that does not match the query).**
//!   Every table that is purged has an index on the timestamp the purge uses,
//!   and every "show me this peer's history" query has an
//!   `(account_id, peer_id, time)` index.
//! * **H6 (`busy_timeout` set on one connection only).** The pragmas are applied
//!   inside [`SCHEMA_SQL`], which [`crate::store::conn::Store`] runs on *every*
//!   connection it opens.
//! * **H8/H9 (dedup by `content_hash` swallows real second edits).** `content_hash`
//!   is unique per account, but the journal is append-only and keyed by
//!   `(account_id, op_seq)`, so two different edits of one message are two rows;
//!   only a byte-identical replay collapses.
//!
//! # Versioning
//!
//! [`SCHEMA_VERSION`] is the version this build *writes*. A store is created at
//! that version directly (the DDL below is the current shape), and an older store
//! is upgraded by [`crate::store::migrate`], which owns the `v1 → v2` step. The
//! rules:
//!
//! 1. every step is **idempotent**: it checks for the column/table/index it wants
//!    before creating it, so a step interrupted by a crash is safe to re-run;
//! 2. every step runs **inside one transaction** together with the
//!    `schema_version` bump, so "table created, version not bumped" cannot happen;
//! 3. a version *newer* than ours is refused, not guessed at: a newer agent may
//!    have added a NOT NULL column this build cannot fill.
//!
//! # What v2 added, and why
//!
//! * `conflicts` — the append-only journal cannot "lose" a write, but it also
//!   cannot tell the client that two devices wrote the same field and one of them
//!   would lose under an HLC-ordered merge. Recording that pair is what makes
//!   `sync.conflicts` possible without reintroducing last-write-wins.
//! * `accounts.conflict_high_water` — the per-account conflict id counter. A
//!   `MAX(conflict_id) + 1` would reuse ids after a prune, which is the L7 bug in
//!   the C++ updater; a stored counter never goes backwards.
//!
//! # Why `synchronous=NORMAL` and not `FULL`
//!
//! Under WAL, `NORMAL` only loses the *tail* of the WAL if the machine loses
//! power; the database stays consistent because commits are still atomic at the
//! WAL level. `FULL` fsyncs the WAL on every commit, which on a small VPS costs
//! one disk flush per ingested event — and this store is written *passively*,
//! many small writes, so `FULL` would turn every captured deletion into a
//! synchronous disk round-trip. The data that must survive a power cut is the
//! *already captured* history; losing the last unflushed batch is acceptable
//! because the client re-sends it (that is exactly what `content_hash`
//! idempotency is for). `docs/architecture.md` § 10.2 says `FULL`; the deviation
//! is deliberate for the passive path and is recorded in
//! `docs/passive-module.md`.
//!
//! `busy_timeout=5000` is not optional: the sync worker and a `VACUUM INTO`
//! backup can overlap, and without it SQLite returns `SQLITE_BUSY` instantly —
//! the H6 failure mode that fed C4.

/// Bumped whenever the DDL below changes. Stored in `schema_meta.schema_version`
/// so `self-test` can refuse to run on a store it does not understand.
///
/// * 1 — passive journal, projections, retention (step 1a).
/// * 2 — `conflicts` + `accounts.conflict_high_water` (`sync.conflicts`, step 3).
/// * 3 — `db_cursor`, the read cursor of the server-side DB ingest
///   (`docs/transport.md` § 15 п. 4, `docs/db-ingest.md`).
pub const SCHEMA_VERSION: u32 = 3;

/// The v1 shape, frozen. Used by the migration test and by nothing else.
///
/// Deliberately **not** edited when the schema changes: its whole purpose is to be
/// a database an older agent wrote, so that "old file → migrate → data intact" is
/// tested against the real old shape rather than against a reconstruction. The DDL
/// is the v1 DDL with comments trimmed; the columns, constraints and indexes are
/// what matters, and [`tests::the_v1_fixture_is_a_strict_subset`] pins that.
pub const SCHEMA_V1_SQL: &str = r#"
PRAGMA journal_mode = WAL;
PRAGMA synchronous = NORMAL;
PRAGMA busy_timeout = 5000;
PRAGMA foreign_keys = ON;
PRAGMA auto_vacuum = INCREMENTAL;

CREATE TABLE IF NOT EXISTS schema_meta (
    key TEXT NOT NULL PRIMARY KEY, value TEXT NOT NULL) WITHOUT ROWID;
INSERT OR IGNORE INTO schema_meta (key, value) VALUES ('schema_version', '1');
INSERT OR IGNORE INTO schema_meta (key, value) VALUES ('format', 'teleforge-passive-store');

CREATE TABLE IF NOT EXISTS accounts (
    account_id BLOB NOT NULL PRIMARY KEY,
    created_at_ms INTEGER NOT NULL,
    last_write_ms INTEGER NOT NULL,
    high_water INTEGER NOT NULL DEFAULT 0,
    device_count INTEGER NOT NULL DEFAULT 0) WITHOUT ROWID;

CREATE TABLE IF NOT EXISTS device_clock (
    account_id BLOB NOT NULL, device_id TEXT NOT NULL,
    last_hlc_physical INTEGER NOT NULL, last_hlc_logical INTEGER NOT NULL,
    last_seen_ms INTEGER NOT NULL,
    PRIMARY KEY (account_id, device_id),
    FOREIGN KEY (account_id) REFERENCES accounts (account_id)) WITHOUT ROWID;

CREATE TABLE IF NOT EXISTS entities (
    account_id BLOB NOT NULL, entity_id TEXT NOT NULL, kind INTEGER NOT NULL,
    first_seen_ms INTEGER NOT NULL, last_seen_ms INTEGER NOT NULL,
    revision_count INTEGER NOT NULL DEFAULT 0, last_op_seq INTEGER NOT NULL DEFAULT 0,
    last_hlc_physical INTEGER NOT NULL DEFAULT 0, last_hlc_logical INTEGER NOT NULL DEFAULT 0,
    deleted INTEGER NOT NULL DEFAULT 0,
    deleted_hlc_physical INTEGER, deleted_hlc_logical INTEGER,
    PRIMARY KEY (account_id, entity_id),
    FOREIGN KEY (account_id) REFERENCES accounts (account_id)) WITHOUT ROWID;
CREATE INDEX IF NOT EXISTS idx_entities_kind_time
    ON entities (account_id, kind, last_seen_ms DESC);

CREATE TABLE IF NOT EXISTS ops (
    op_seq INTEGER NOT NULL, account_id BLOB NOT NULL, kind INTEGER NOT NULL,
    entity_id TEXT NOT NULL, field TEXT, value BLOB NOT NULL,
    hlc_physical INTEGER NOT NULL, hlc_logical INTEGER NOT NULL,
    device_id TEXT NOT NULL, content_hash BLOB NOT NULL, applied_at_ms INTEGER NOT NULL,
    UNIQUE (account_id, op_seq), UNIQUE (account_id, content_hash),
    FOREIGN KEY (account_id) REFERENCES accounts (account_id));
CREATE INDEX IF NOT EXISTS idx_ops_entity_time
    ON ops (account_id, entity_id, hlc_physical, hlc_logical);
CREATE INDEX IF NOT EXISTS idx_ops_applied ON ops (applied_at_ms);

CREATE TABLE IF NOT EXISTS presence (
    account_id BLOB NOT NULL, op_seq INTEGER NOT NULL, peer_id INTEGER NOT NULL,
    status INTEGER NOT NULL, detail_ms INTEGER, observed_ms INTEGER NOT NULL,
    hlc_physical INTEGER NOT NULL, hlc_logical INTEGER NOT NULL,
    device_id TEXT NOT NULL, content_hash BLOB NOT NULL,
    PRIMARY KEY (account_id, op_seq),
    FOREIGN KEY (account_id) REFERENCES accounts (account_id)) WITHOUT ROWID;
CREATE INDEX IF NOT EXISTS idx_presence_peer_time
    ON presence (account_id, peer_id, observed_ms DESC, op_seq DESC);
CREATE INDEX IF NOT EXISTS idx_presence_observed ON presence (observed_ms);

CREATE TABLE IF NOT EXISTS profiles (
    account_id BLOB NOT NULL, op_seq INTEGER NOT NULL, peer_id INTEGER NOT NULL,
    field TEXT NOT NULL, value BLOB NOT NULL, observed_ms INTEGER NOT NULL,
    hlc_physical INTEGER NOT NULL, hlc_logical INTEGER NOT NULL,
    device_id TEXT NOT NULL, content_hash BLOB NOT NULL,
    PRIMARY KEY (account_id, op_seq),
    FOREIGN KEY (account_id) REFERENCES accounts (account_id)) WITHOUT ROWID;
CREATE INDEX IF NOT EXISTS idx_profiles_peer_field_time
    ON profiles (account_id, peer_id, field, observed_ms DESC, op_seq DESC);
CREATE INDEX IF NOT EXISTS idx_profiles_observed ON profiles (observed_ms);

CREATE TABLE IF NOT EXISTS archive_obs (
    account_id BLOB NOT NULL, op_seq INTEGER NOT NULL, peer_id INTEGER NOT NULL,
    source TEXT NOT NULL, payload BLOB NOT NULL, observed_ms INTEGER NOT NULL,
    hlc_physical INTEGER NOT NULL, hlc_logical INTEGER NOT NULL,
    device_id TEXT NOT NULL, content_hash BLOB NOT NULL,
    PRIMARY KEY (account_id, op_seq),
    FOREIGN KEY (account_id) REFERENCES accounts (account_id)) WITHOUT ROWID;
CREATE INDEX IF NOT EXISTS idx_archive_peer_time
    ON archive_obs (account_id, peer_id, observed_ms DESC);
CREATE INDEX IF NOT EXISTS idx_archive_observed ON archive_obs (observed_ms);
"#;

/// Everything needed to create a store, applied with `execute_batch`.
///
/// `INSERT OR IGNORE` on `schema_meta` keeps this idempotent: opening an existing
/// store re-runs the whole batch and changes nothing. On a v1 store the version row
/// already exists, so `OR IGNORE` leaves `1` in place and
/// [`crate::store::migrate`] is what moves it to 2.
pub const SCHEMA_SQL: &str = r#"
PRAGMA journal_mode = WAL;
PRAGMA synchronous = NORMAL;
PRAGMA busy_timeout = 5000;
PRAGMA foreign_keys = ON;
-- Incremental auto-vacuum lets `purge_by_size` give pages back without a full
-- VACUUM (which would need an exclusive lock and double the disk usage).
PRAGMA auto_vacuum = INCREMENTAL;

CREATE TABLE IF NOT EXISTS schema_meta (
    key   TEXT NOT NULL PRIMARY KEY,
    value TEXT NOT NULL
) WITHOUT ROWID;
INSERT OR IGNORE INTO schema_meta (key, value) VALUES ('schema_version', '3');
INSERT OR IGNORE INTO schema_meta (key, value) VALUES ('format', 'teleforge-passive-store');

-- The account registry. `high_water` is the last `op_seq` handed out for this
-- account: one counter row per account, bumped in the same transaction as the
-- insert (C4: never "sequence handed out, row missing").
-- `conflict_high_water` (v2) is the same idea for conflict ids: never reused, not
-- even after a prune, because a client may still hold an id it saw earlier.
CREATE TABLE IF NOT EXISTS accounts (
    account_id            BLOB    NOT NULL PRIMARY KEY,
    created_at_ms         INTEGER NOT NULL,
    last_write_ms         INTEGER NOT NULL,
    high_water            INTEGER NOT NULL DEFAULT 0,
    device_count          INTEGER NOT NULL DEFAULT 0,
    conflict_high_water   INTEGER NOT NULL DEFAULT 0
) WITHOUT ROWID;

-- Per-device clock watermark. The HLC monotonicity check is per (account,
-- device): a device that goes backwards is a bug on the client, and accepting its
-- events would reorder them behind writes we already returned as applied.
CREATE TABLE IF NOT EXISTS device_clock (
    account_id        BLOB    NOT NULL,
    device_id         TEXT    NOT NULL,
    last_hlc_physical INTEGER NOT NULL,
    last_hlc_logical  INTEGER NOT NULL,
    last_seen_ms      INTEGER NOT NULL,
    PRIMARY KEY (account_id, device_id),
    FOREIGN KEY (account_id) REFERENCES accounts (account_id)
) WITHOUT ROWID;

-- Projection of an entity's *aggregates*. Never a value: the values live in the
-- journal. `revision_count` and `last_seen_ms` are monotone, `deleted` is derived
-- from the newest op, so nothing here can "win" over a newer op (H7).
CREATE TABLE IF NOT EXISTS entities (
    account_id        BLOB    NOT NULL,
    entity_id         TEXT    NOT NULL,
    kind              INTEGER NOT NULL,
    first_seen_ms     INTEGER NOT NULL,
    last_seen_ms      INTEGER NOT NULL,
    revision_count    INTEGER NOT NULL DEFAULT 0,
    last_op_seq       INTEGER NOT NULL DEFAULT 0,
    last_hlc_physical INTEGER NOT NULL DEFAULT 0,
    last_hlc_logical  INTEGER NOT NULL DEFAULT 0,
    deleted           INTEGER NOT NULL DEFAULT 0,
    deleted_hlc_physical INTEGER,
    deleted_hlc_logical  INTEGER,
    PRIMARY KEY (account_id, entity_id),
    FOREIGN KEY (account_id) REFERENCES accounts (account_id)
) WITHOUT ROWID;
CREATE INDEX IF NOT EXISTS idx_entities_kind_time
    ON entities (account_id, kind, last_seen_ms DESC);

-- The append-only journal: one row per event, forever. This is the source of
-- truth; every other table can be rebuilt from it.
CREATE TABLE IF NOT EXISTS ops (
    op_seq        INTEGER NOT NULL,
    account_id    BLOB    NOT NULL,
    kind          INTEGER NOT NULL,
    entity_id     TEXT    NOT NULL,
    field         TEXT,
    value         BLOB    NOT NULL,
    hlc_physical  INTEGER NOT NULL,
    hlc_logical   INTEGER NOT NULL,
    device_id     TEXT    NOT NULL,
    content_hash  BLOB    NOT NULL,
    applied_at_ms INTEGER NOT NULL,
    -- Monotone per account, as the design requires (never global, never reused).
    UNIQUE (account_id, op_seq),
    -- Idempotency. Per account: the same bytes captured under two accounts are
    -- two facts, not one.
    UNIQUE (account_id, content_hash),
    FOREIGN KEY (account_id) REFERENCES accounts (account_id)
);
CREATE INDEX IF NOT EXISTS idx_ops_entity_time
    ON ops (account_id, entity_id, hlc_physical, hlc_logical);
CREATE INDEX IF NOT EXISTS idx_ops_applied ON ops (applied_at_ms);

-- Presence history (replaces `OnlineEvent`, which had no `account_id` and one
-- row per poll). `status` is the `EventKind` code so a history row cannot drift
-- from the journal; `detail_ms` is the `online_till`/`last_seen` payload, NULL
-- when the client did not know it.
CREATE TABLE IF NOT EXISTS presence (
    account_id    BLOB    NOT NULL,
    op_seq        INTEGER NOT NULL,
    peer_id       INTEGER NOT NULL,
    status        INTEGER NOT NULL,
    detail_ms     INTEGER,
    observed_ms   INTEGER NOT NULL,
    hlc_physical  INTEGER NOT NULL,
    hlc_logical   INTEGER NOT NULL,
    device_id     TEXT    NOT NULL,
    content_hash  BLOB    NOT NULL,
    PRIMARY KEY (account_id, op_seq),
    FOREIGN KEY (account_id) REFERENCES accounts (account_id)
) WITHOUT ROWID;
CREATE INDEX IF NOT EXISTS idx_presence_peer_time
    ON presence (account_id, peer_id, observed_ms DESC, op_seq DESC);
CREATE INDEX IF NOT EXISTS idx_presence_observed ON presence (observed_ms);

-- Profile history (replaces `PeerArchiveName/Username/Bio`, which grew one row
-- per observation but had no account column and no way to ask "what did it look
-- like at time T"). The *current* value is a query, not a column: no LWW.
CREATE TABLE IF NOT EXISTS profiles (
    account_id    BLOB    NOT NULL,
    op_seq        INTEGER NOT NULL,
    peer_id       INTEGER NOT NULL,
    field         TEXT    NOT NULL,
    value         BLOB    NOT NULL,
    observed_ms   INTEGER NOT NULL,
    hlc_physical  INTEGER NOT NULL,
    hlc_logical   INTEGER NOT NULL,
    device_id     TEXT    NOT NULL,
    content_hash  BLOB    NOT NULL,
    PRIMARY KEY (account_id, op_seq),
    FOREIGN KEY (account_id) REFERENCES accounts (account_id)
) WITHOUT ROWID;
CREATE INDEX IF NOT EXISTS idx_profiles_peer_field_time
    ON profiles (account_id, peer_id, field, observed_ms DESC, op_seq DESC);
CREATE INDEX IF NOT EXISTS idx_profiles_observed ON profiles (observed_ms);

-- Archive observations: "this peer was seen as the author of a message in this
-- chat", "was a participant", "the chat was opened". `payload` is opaque.
CREATE TABLE IF NOT EXISTS archive_obs (
    account_id    BLOB    NOT NULL,
    op_seq        INTEGER NOT NULL,
    peer_id       INTEGER NOT NULL,
    source        TEXT    NOT NULL,
    payload       BLOB    NOT NULL,
    observed_ms   INTEGER NOT NULL,
    hlc_physical  INTEGER NOT NULL,
    hlc_logical   INTEGER NOT NULL,
    device_id     TEXT    NOT NULL,
    content_hash  BLOB    NOT NULL,
    PRIMARY KEY (account_id, op_seq),
    FOREIGN KEY (account_id) REFERENCES accounts (account_id)
) WITHOUT ROWID;
CREATE INDEX IF NOT EXISTS idx_archive_peer_time
    ON archive_obs (account_id, peer_id, observed_ms DESC);
CREATE INDEX IF NOT EXISTS idx_archive_observed ON archive_obs (observed_ms);
"#;

/// The v2 additions, as a separate batch.
///
/// Kept apart from [`SCHEMA_SQL`] because they are applied *twice* on purpose: once
/// on every open (cheap `IF NOT EXISTS`) and once by
/// [`crate::store::migrate`] inside the version-bump transaction. A fresh store gets
/// them from here, an old store from the migration, and there is exactly one copy
/// of the DDL either way.
pub const SCHEMA_V2_SQL: &str = r#"
-- Conflicts the agent refuses to resolve silently.
--
-- The journal keeps *both* values, so nothing is lost; this table is the index
-- that lets `sync.conflicts` say "these two writes for the same field came from
-- different devices and an HLC-ordered merge would drop one". `local_*` is what
-- the store already had, `remote_*` is the write that arrived. `resolved_by` stays
-- NULL until a human (through the client) decides — the agent never picks a
-- winner, which is finding H7.
CREATE TABLE IF NOT EXISTS conflicts (
    account_id        BLOB    NOT NULL,
    conflict_id       INTEGER NOT NULL,
    entity_id         TEXT    NOT NULL,
    field             TEXT,
    kind              INTEGER NOT NULL,
    local_op_seq      INTEGER NOT NULL,
    local_hlc_physical INTEGER NOT NULL,
    local_hlc_logical  INTEGER NOT NULL,
    local_device      TEXT    NOT NULL,
    remote_op_seq     INTEGER NOT NULL,
    remote_hlc_physical INTEGER NOT NULL,
    remote_hlc_logical  INTEGER NOT NULL,
    remote_device     TEXT    NOT NULL,
    detected_at_ms    INTEGER NOT NULL,
    resolved_by       TEXT,
    PRIMARY KEY (account_id, conflict_id),
    FOREIGN KEY (account_id) REFERENCES accounts (account_id)
) WITHOUT ROWID;
-- Serves "the unresolved conflicts after id N", which is what the client polls.
CREATE INDEX IF NOT EXISTS idx_conflicts_detected
    ON conflicts (account_id, detected_at_ms ASC, conflict_id ASC);
"#;

/// The v3 additions: the server-side DB read cursor.
///
/// # Why a second cursor and not `device_clock` / `accounts.high_water`
///
/// Those two are about *devices* and about the journal: `device_clock` bounds the
/// HLC of a client device, `accounts.high_water` is the last `op_seq` handed out.
/// Neither answers "which row of the user's PostgreSQL/MySQL did we already
/// publish", and overloading either would make one counter jump when the other
/// moves. So the DB read position gets its own table, keyed by
/// `(account_id, source)`: `source` is the table the cursor belongs to, because
/// two different sync tables have incomparable key spaces and one cursor cannot
/// describe both.
///
/// The row is written **in the same SQLite transaction as the `ops` rows it
/// accounts for** ([`crate::db::ingest`]), so "events published, cursor not
/// advanced" is not a state that can exist — that is the C4 rule applied to the
/// other direction of the same problem. `last_key` is additionally monotone in
/// SQL (`CASE WHEN excluded.last_key > last_key`), so even a caller that tries to
/// move it backwards cannot.
pub const SCHEMA_V3_SQL: &str = r#"
CREATE TABLE IF NOT EXISTS db_cursor (
    account_id BLOB    NOT NULL,
    source     TEXT    NOT NULL,
    last_key   INTEGER NOT NULL,
    updated_at INTEGER NOT NULL,
    PRIMARY KEY (account_id, source),
    FOREIGN KEY (account_id) REFERENCES accounts (account_id)
) WITHOUT ROWID;
"#;

/// Tables a healthy store must contain. Used by `self-test`; a missing entry is a
/// store written by an older build, not a warning to ignore.
pub const REQUIRED_TABLES: [&str; 10] = [
    "schema_meta",
    "accounts",
    "device_clock",
    "entities",
    "ops",
    "presence",
    "profiles",
    "archive_obs",
    "conflicts",
    "db_cursor",
];

/// Indexes a healthy store must contain. Each one exists for a query that the
/// passive path actually issues, so a missing index is a silent full scan.
pub const REQUIRED_INDEXES: [&str; 10] = [
    "idx_entities_kind_time",
    "idx_ops_entity_time",
    "idx_ops_applied",
    "idx_presence_peer_time",
    "idx_presence_observed",
    "idx_profiles_peer_field_time",
    "idx_profiles_observed",
    "idx_archive_peer_time",
    "idx_archive_observed",
    "idx_conflicts_detected",
];

/// Columns added by v2 that a v1 file does not have. [`crate::store::migrate`]
/// asks SQLite for the actual list rather than assuming.
pub const V2_ADDED_COLUMNS: [(&str, &str); 1] = [("accounts", "conflict_high_water")];

#[cfg(test)]
mod tests {
    use super::*;

    /// The whole current DDL: the base batch plus the additive ones. Tests that ask
    /// "does the schema declare X" must look at all of them, otherwise a v2/v3
    /// addition is invisible to them.
    fn current_ddl() -> String {
        format!("{SCHEMA_SQL}\n{SCHEMA_V2_SQL}\n{SCHEMA_V3_SQL}")
    }

    #[test]
    fn every_table_that_holds_account_data_declares_account_id() {
        // The C3 fix, asserted rather than promised.
        let ddl = current_ddl();
        for table in [
            "device_clock",
            "entities",
            "ops",
            "presence",
            "profiles",
            "archive_obs",
            "conflicts",
            "db_cursor",
        ] {
            let marker = format!("CREATE TABLE IF NOT EXISTS {table} (");
            let start = ddl
                .find(&marker)
                .unwrap_or_else(|| panic!("{table} is missing from the DDL"));
            let body = &ddl[start + marker.len()..];
            let end = body.find(';').expect("table declaration terminated");
            assert!(
                body[..end].contains("account_id"),
                "{table} must carry account_id"
            );
            assert!(
                body[..end].contains("REFERENCES accounts"),
                "{table} must be bound to a registered account"
            );
        }
    }

    #[test]
    fn op_seq_is_unique_per_account_not_globally() {
        assert!(SCHEMA_SQL.contains("UNIQUE (account_id, op_seq)"));
        assert!(!SCHEMA_SQL.contains("op_seq INTEGER PRIMARY KEY"));
    }

    #[test]
    fn content_hash_is_unique_per_account() {
        assert!(SCHEMA_SQL.contains("UNIQUE (account_id, content_hash)"));
    }

    #[test]
    fn pragmas_that_h6_needed_are_in_the_schema_batch() {
        // They must be applied through the same batch every connection runs,
        // otherwise "busy_timeout was set once" is exactly the C++ bug again.
        assert!(SCHEMA_SQL.contains("PRAGMA busy_timeout = 5000"));
        assert!(SCHEMA_SQL.contains("PRAGMA journal_mode = WAL"));
        assert!(SCHEMA_SQL.contains("PRAGMA synchronous = NORMAL"));
        assert!(SCHEMA_SQL.contains("PRAGMA foreign_keys = ON"));
    }

    #[test]
    fn required_lists_match_the_ddl() {
        let ddl = current_ddl();
        for table in REQUIRED_TABLES {
            assert!(
                ddl.contains(&format!("CREATE TABLE IF NOT EXISTS {table} (")),
                "{table} is listed as required but missing from the DDL"
            );
        }
        for index in REQUIRED_INDEXES {
            assert!(
                ddl.contains(&format!("CREATE INDEX IF NOT EXISTS {index}")),
                "{index} is listed as required but missing from the DDL"
            );
        }
    }

    #[test]
    fn schema_version_row_matches_the_constant() {
        assert!(SCHEMA_SQL.contains(&format!("VALUES ('schema_version', '{SCHEMA_VERSION}')")));
        assert!(SCHEMA_V1_SQL.contains("VALUES ('schema_version', '1')"));
    }

    #[test]
    fn the_v1_fixture_is_a_strict_subset() {
        // The fixture must not accidentally contain a later addition: a migration
        // test against a fixture that already has the new column proves nothing.
        assert!(!SCHEMA_V1_SQL.contains("conflicts"));
        assert!(!SCHEMA_V1_SQL.contains("conflict_high_water"));
        assert!(!SCHEMA_V1_SQL.contains("db_cursor"));
        for column in V2_ADDED_COLUMNS {
            assert!(
                SCHEMA_SQL.contains(column.1),
                "{} must exist in the current DDL",
                column.1
            );
        }
        // Every v1 table still exists today: nothing was dropped, only added.
        for table in [
            "schema_meta",
            "accounts",
            "device_clock",
            "entities",
            "ops",
            "presence",
            "profiles",
            "archive_obs",
        ] {
            assert!(SCHEMA_V1_SQL.contains(&format!("CREATE TABLE IF NOT EXISTS {table} (")));
            assert!(SCHEMA_SQL.contains(&format!("CREATE TABLE IF NOT EXISTS {table} (")));
        }
    }
}
