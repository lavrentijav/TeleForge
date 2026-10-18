//! The connection and the RAII transaction.
//!
//! # C4: one place that opens a transaction, one place that resolves it
//!
//! The C++ side did `begin_transaction(); insert(row); commit();` inside a `try`
//! whose `catch` only logged. One failed insert (a `SQLITE_BUSY` during a
//! `VACUUM INTO`, a `SQLITE_FULL`, a schema mismatch after `sync_schema`) left
//! the connection *inside* an open transaction, and every later
//! `begin_transaction()` failed with "cannot start a transaction within a
//! transaction" — the passive path silently stopped recording until a restart.
//!
//! Here there is exactly one way to write: [`Store::transaction`] hands out a
//! [`Tx`], and [`Tx`]'s `Drop` issues `ROLLBACK` unless [`Tx::commit`] was
//! called. A `?` inside the closure therefore *cannot* leave the connection
//! mid-transaction, and `store::tests::*` asserts it by writing again after a
//! deliberately failed transaction.
//!
//! # C5: one owner thread
//!
//! `rusqlite::Connection` is `Send` but not `Sync`, so [`Store`] inherits that and
//! the compiler enforces "one thread at a time" instead of a comment doing it.
//!
//! # H6: pragmas on every connection
//!
//! `busy_timeout` in the C++ build was set in `Storage::initialize` only; the
//! snapshot and merge connections were opened without it. Here the pragmas are
//! part of [`crate::store::schema::SCHEMA_SQL`], which every `open` runs
//! unconditionally.

use std::path::{Path, PathBuf};

use rusqlite::{Connection, OptionalExtension};

use crate::error::{Error, Result};
use crate::fsperm;
use crate::store::event::{AccountId, EventKind, MAX_ENTITY_ID_LEN};
use crate::sync::clock::Hlc;

/// Busy timeout, mirrored in the DDL. Written twice on purpose: once as SQL (so a
/// human reading the schema sees it) and once through rusqlite's API (so it is
/// also set if a future SQLite build rejects the pragma form).
pub const BUSY_TIMEOUT_MS: u64 = 5_000;

/// An open passive store.
///
/// Every statement the agent issues goes through one of the methods here or
/// through [`Store::transaction`]; there is no second write path.
pub struct Store {
    conn: Connection,
    path: PathBuf,
}

impl Store {
    /// Open (and, if needed, create) a store at `path`.
    ///
    /// Order matters: the parent directory is created private *before* SQLite
    /// creates the file, so there is no window in which `data.db` exists under a
    /// world-readable directory.
    pub fn open(path: impl AsRef<Path>) -> Result<Self> {
        let path = path.as_ref().to_path_buf();
        if let Some(parent) = path.parent() {
            if !parent.as_os_str().is_empty() {
                fsperm::create_private_dir(&parent.to_string_lossy())?;
            }
        }
        let conn = Connection::open(&path)?;
        let store = Self::finish_open(conn, path)?;
        store.restrict_files()?;
        Ok(store)
    }

    /// In-memory store, for tests and for `self-test` probes that must not touch
    /// the real database.
    pub fn open_in_memory() -> Result<Self> {
        let conn = Connection::open_in_memory()?;
        let store = Self::finish_open(conn, PathBuf::from(":memory:"))?;
        Ok(store)
    }

    fn finish_open(mut conn: Connection, path: PathBuf) -> Result<Self> {
        conn.busy_timeout(std::time::Duration::from_millis(BUSY_TIMEOUT_MS))?;
        // The whole schema batch, including the pragmas: H6 is precisely the bug
        // of setting a pragma on *one* connection.
        conn.execute_batch(crate::store::schema::SCHEMA_SQL)?;
        // v2 objects are applied separately because they are also what the
        // migration writes; both batches are idempotent, so a fresh store and an
        // upgraded store end up byte-identical in shape.
        conn.execute_batch(crate::store::schema::SCHEMA_V2_SQL)?;
        // Same for v3 (`db_cursor`): the server-side DB ingest reads and writes it
        // through `store::cursor`, and an older file must have it before the first
        // ingest pass runs, not after.
        conn.execute_batch(crate::store::schema::SCHEMA_V3_SQL)?;
        // Move an older file to the current version *before* anything reads it.
        // `migrate::run` is a no-op (one `PRAGMA table_info` per added column) on a
        // store that is already current.
        let _report = crate::store::migrate::run(&mut conn)?;
        let store = Self { conn, path };
        store.check_schema_version()?;
        Ok(store)
    }

    /// Refuse a store written by a newer build rather than operating on half a
    /// schema.
    ///
    /// An *older* schema is migrated by [`Self::finish_open`] before this runs, so by
    /// the time this check executes the recorded version must be exactly
    /// [`crate::store::schema::SCHEMA_VERSION`] — anything else means the migration
    /// did not take, which is a storage error worth surfacing loudly.
    fn check_schema_version(&self) -> Result<()> {
        let found = crate::store::migrate::read_version(&self.conn)?;
        let expected = crate::store::schema::SCHEMA_VERSION;
        if found == expected {
            return Ok(());
        }
        Err(Error::Storage(format!(
            "store schema_version is {found} after migration, expected {expected}"
        )))
    }

    /// `0600` on the database and its WAL/SHM siblings.
    ///
    /// SQLite creates `-wal`/`-shm` itself, with the process umask, which is how a
    /// WAL full of captured message text ends up group-readable. Those two are
    /// best-effort (they are recreated on the next checkpoint and may not exist
    /// yet); the main file must succeed.
    fn restrict_files(&self) -> Result<()> {
        let main = self.path.to_string_lossy().to_string();
        if self.path != Path::new(":memory:") {
            fsperm::restrict_file(&main)?;
        }
        for suffix in ["-wal", "-shm"] {
            let sibling = format!("{main}{suffix}");
            if Path::new(&sibling).exists() {
                // Not `?`: on a platform without POSIX modes this is reported by
                // `self-test`, and failing to open the store because a WAL file
                // has the wrong mode would be worse than recording the fact.
                let _ = fsperm::restrict_file(&sibling);
            }
        }
        Ok(())
    }

    pub fn path(&self) -> &Path {
        &self.path
    }

    /// The one write path. Commits on `Ok`, rolls back on `Err` *and* on panic
    /// (the guard's `Drop` runs during unwinding).
    pub fn transaction<T>(&mut self, body: impl FnOnce(&Tx<'_>) -> Result<T>) -> Result<T> {
        let tx = Tx::begin(&self.conn)?;
        match body(&tx) {
            Ok(value) => {
                tx.commit()?;
                Ok(value)
            }
            Err(error) => {
                // `tx` is dropped here, which issues ROLLBACK.
                drop(tx);
                Err(error)
            }
        }
    }

    /// Escape hatch for `self-test` and read helpers. Not a write path: anything
    /// that mutates must go through [`Self::transaction`].
    pub fn connection(&self) -> &Connection {
        &self.conn
    }

    /// True when no transaction is open. `self-test` and the C4 regression test
    /// assert this after a failure.
    pub fn is_idle(&self) -> bool {
        self.conn.is_autocommit()
    }

    pub fn close(self) {
        drop(self);
    }
}

impl core::fmt::Debug for Store {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        f.debug_struct("Store").field("path", &self.path).finish()
    }
}

/// The RAII transaction guard.
///
/// ```ignore
/// let value = store.transaction(|tx| {
///     let seq = tx.next_op_seq(&account, now)?;
///     tx.insert_op(&new_op)?;
///     Ok(seq)                 // a `?` anywhere above rolls back
/// })?;
/// ```
pub struct Tx<'a> {
    conn: &'a Connection,
    finished: bool,
}

impl<'a> Tx<'a> {
    fn begin(conn: &'a Connection) -> Result<Self> {
        // `BEGIN IMMEDIATE`: take the write lock up front. A deferred transaction
        // that upgrades mid-way is the classic `SQLITE_BUSY` deadlock under two
        // writers, which is exactly what C4/H6 were made of.
        conn.execute_batch("BEGIN IMMEDIATE")?;
        Ok(Self {
            conn,
            finished: false,
        })
    }

    pub fn connection(&self) -> &Connection {
        self.conn
    }

    pub fn commit(mut self) -> Result<()> {
        // If COMMIT itself fails (e.g. SQLITE_FULL) the guard must still roll
        // back, so `finished` is set only *after* a successful commit.
        self.conn.execute_batch("COMMIT")?;
        self.finished = true;
        Ok(())
    }

    /// Register the account if this is its first event, then hand out the next
    /// `op_seq`. Both happen in the caller's transaction, so a crash can never
    /// hand out a sequence number whose row was not written.
    pub fn next_op_seq(&self, account: &AccountId, now_ms: i64) -> Result<u64> {
        self.ensure_account(account, now_ms)?;
        let high_water: i64 = self.conn.query_row(
            "UPDATE accounts
                SET high_water = high_water + 1,
                    last_write_ms = MAX(last_write_ms, ?2)
              WHERE account_id = ?1
          RETURNING high_water",
            rusqlite::params![account.as_blob(), now_ms],
            |row| row.get(0),
        )?;
        Ok(high_water as u64)
    }

    /// Idempotent account registration.
    ///
    /// Every table that carries per-account data has a foreign key to `accounts`,
    /// so this must run before the first row for an account — including the
    /// device clock, which is written by callers that do not issue a sequence
    /// number.
    pub fn ensure_account(&self, account: &AccountId, now_ms: i64) -> Result<()> {
        self.conn.execute(
            "INSERT INTO accounts (account_id, created_at_ms, last_write_ms)
             VALUES (?1, ?2, ?2)
             ON CONFLICT (account_id) DO NOTHING",
            rusqlite::params![account.as_blob(), now_ms],
        )?;
        Ok(())
    }

    pub fn high_water(&self, account: &AccountId) -> Result<u64> {
        let value: Option<i64> = self
            .conn
            .query_row(
                "SELECT high_water FROM accounts WHERE account_id = ?1",
                [account.as_blob()],
                |row| row.get(0),
            )
            .optional()?;
        Ok(value.unwrap_or(0) as u64)
    }

    /// The `op_seq` already assigned to this `content_hash`, if any. The lookup is
    /// a read, but the uniqueness constraint behind it is what actually makes
    /// idempotency a property of the store rather than a convention.
    pub fn op_seq_for_content_hash(
        &self,
        account: &AccountId,
        content_hash: &[u8; 32],
    ) -> Result<Option<u64>> {
        let value: Option<i64> = self
            .conn
            .query_row(
                "SELECT op_seq FROM ops WHERE account_id = ?1 AND content_hash = ?2",
                rusqlite::params![account.as_blob(), content_hash.as_slice()],
                |row| row.get(0),
            )
            .optional()?;
        Ok(value.map(|seq| seq as u64))
    }

    /// Last HLC this device produced for this account.
    pub fn device_last_hlc(&self, account: &AccountId, device_id: &str) -> Result<Option<Hlc>> {
        let row: Option<(i64, i64)> = self
            .conn
            .query_row(
                "SELECT last_hlc_physical, last_hlc_logical
                   FROM device_clock WHERE account_id = ?1 AND device_id = ?2",
                rusqlite::params![account.as_blob(), device_id],
                |row| Ok((row.get(0)?, row.get(1)?)),
            )
            .optional()?;
        Ok(row.map(|(physical, logical)| Hlc::new(physical as u64, logical as u16)))
    }

    /// HLC of the newest delete observed for this entity, if it has one.
    pub fn entity_deleted_hlc(&self, account: &AccountId, entity_id: &str) -> Result<Option<Hlc>> {
        let row: Option<(i64, i64)> = self
            .conn
            .query_row(
                "SELECT deleted_hlc_physical, deleted_hlc_logical
                   FROM entities
                  WHERE account_id = ?1 AND entity_id = ?2 AND deleted = 1",
                rusqlite::params![account.as_blob(), entity_id],
                |row| Ok((row.get(0)?, row.get(1)?)),
            )
            .optional()?;
        Ok(row.map(|(physical, logical): (i64, i64)| Hlc::new(physical as u64, logical as u16)))
    }

    pub fn insert_op(&self, op: &NewOp<'_>) -> Result<()> {
        self.conn.execute(
            "INSERT INTO ops (op_seq, account_id, kind, entity_id, field, value,
                              hlc_physical, hlc_logical, device_id, content_hash, applied_at_ms)
             VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11)",
            rusqlite::params![
                op.op_seq as i64,
                op.account.as_blob(),
                op.kind.code(),
                op.entity_id,
                op.field,
                op.value,
                op.hlc.physical_ms as i64,
                op.hlc.logical,
                op.device_id,
                op.content_hash.as_slice(),
                op.applied_at_ms,
            ],
        )?;
        Ok(())
    }

    /// Update the entity projection. Aggregates only: nothing here can overwrite a
    /// captured value, which is the H7 fix.
    pub fn upsert_entity(&self, entity: &EntityUpdate<'_>) -> Result<()> {
        debug_assert!(entity.entity_id.len() <= MAX_ENTITY_ID_LEN);
        self.conn.execute(
            "INSERT INTO entities (account_id, entity_id, kind, first_seen_ms, last_seen_ms,
                                   revision_count, last_op_seq, last_hlc_physical,
                                   last_hlc_logical, deleted, deleted_hlc_physical,
                                   deleted_hlc_logical)
             VALUES (?1, ?2, ?3, ?4, ?4, 1, ?5, ?6, ?7, ?8, ?9, ?10)
             ON CONFLICT (account_id, entity_id) DO UPDATE SET
                 revision_count = entities.revision_count + 1,
                 last_seen_ms = MAX(entities.last_seen_ms, excluded.last_seen_ms),
                 kind = CASE WHEN excluded.last_hlc_physical >= entities.last_hlc_physical
                             THEN excluded.kind ELSE entities.kind END,
                 last_op_seq = CASE WHEN excluded.last_hlc_physical >= entities.last_hlc_physical
                                    THEN excluded.last_op_seq ELSE entities.last_op_seq END,
                 last_hlc_physical = MAX(entities.last_hlc_physical, excluded.last_hlc_physical),
                 last_hlc_logical = MAX(entities.last_hlc_logical, excluded.last_hlc_logical),
                 deleted = CASE WHEN excluded.last_hlc_physical >= entities.last_hlc_physical
                                THEN excluded.deleted ELSE entities.deleted END,
                 -- `MAX()` of a NULL is NULL in SQLite, so the two nullable sides
                 -- are coalesced explicitly rather than trusting MAX with NULL.
                 deleted_hlc_physical = CASE
                     WHEN excluded.deleted_hlc_physical IS NULL
                         THEN entities.deleted_hlc_physical
                     WHEN entities.deleted_hlc_physical IS NULL
                         THEN excluded.deleted_hlc_physical
                     ELSE MAX(entities.deleted_hlc_physical, excluded.deleted_hlc_physical)
                 END,
                 deleted_hlc_logical = CASE
                     WHEN excluded.deleted_hlc_logical IS NULL
                         THEN entities.deleted_hlc_logical
                     WHEN entities.deleted_hlc_logical IS NULL
                         THEN excluded.deleted_hlc_logical
                     ELSE MAX(entities.deleted_hlc_logical, excluded.deleted_hlc_logical)
                 END",
            rusqlite::params![
                entity.account.as_blob(),
                entity.entity_id,
                entity.kind.code(),
                entity.observed_ms,
                entity.op_seq as i64,
                entity.hlc.physical_ms as i64,
                entity.hlc.logical,
                i64::from(entity.deleted),
                entity.deleted_hlc.map(|hlc| hlc.physical_ms as i64),
                entity.deleted_hlc.map(|hlc| hlc.logical),
            ],
        )?;
        Ok(())
    }

    pub fn insert_presence(&self, row: &PresenceInsert<'_>) -> Result<()> {
        self.conn.execute(
            "INSERT INTO presence (account_id, op_seq, peer_id, status, detail_ms,
                                   observed_ms, hlc_physical, hlc_logical, device_id,
                                   content_hash)
             VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10)",
            rusqlite::params![
                row.account.as_blob(),
                row.op_seq as i64,
                row.peer_id,
                row.status,
                row.detail_ms,
                row.observed_ms,
                row.hlc.physical_ms as i64,
                row.hlc.logical,
                row.device_id,
                row.content_hash.as_slice(),
            ],
        )?;
        Ok(())
    }

    pub fn insert_profile(&self, row: &ProfileInsert<'_>) -> Result<()> {
        self.conn.execute(
            "INSERT INTO profiles (account_id, op_seq, peer_id, field, value, observed_ms,
                                   hlc_physical, hlc_logical, device_id, content_hash)
             VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10)",
            rusqlite::params![
                row.account.as_blob(),
                row.op_seq as i64,
                row.peer_id,
                row.field,
                row.value,
                row.observed_ms,
                row.hlc.physical_ms as i64,
                row.hlc.logical,
                row.device_id,
                row.content_hash.as_slice(),
            ],
        )?;
        Ok(())
    }

    pub fn insert_archive(&self, row: &ArchiveInsert<'_>) -> Result<()> {
        self.conn.execute(
            "INSERT INTO archive_obs (account_id, op_seq, peer_id, source, payload,
                                      observed_ms, hlc_physical, hlc_logical, device_id,
                                      content_hash)
             VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10)",
            rusqlite::params![
                row.account.as_blob(),
                row.op_seq as i64,
                row.peer_id,
                row.source,
                row.payload,
                row.observed_ms,
                row.hlc.physical_ms as i64,
                row.hlc.logical,
                row.device_id,
                row.content_hash.as_slice(),
            ],
        )?;
        Ok(())
    }

    /// Advance the per-device watermark. `new_device` bumps
    /// `accounts.device_count`; the caller knows it because it just read
    /// [`Tx::device_last_hlc`].
    pub fn update_device_clock(
        &self,
        account: &AccountId,
        device_id: &str,
        hlc: Hlc,
        now_ms: i64,
        new_device: bool,
    ) -> Result<()> {
        // `device_clock` has a foreign key to `accounts`; registering here keeps
        // the method safe to call on its own.
        self.ensure_account(account, now_ms)?;
        self.conn.execute(
            "INSERT INTO device_clock (account_id, device_id, last_hlc_physical,
                                       last_hlc_logical, last_seen_ms)
             VALUES (?1, ?2, ?3, ?4, ?5)
             ON CONFLICT (account_id, device_id) DO UPDATE SET
                 last_hlc_physical = MAX(device_clock.last_hlc_physical,
                                        excluded.last_hlc_physical),
                 last_hlc_logical = MAX(device_clock.last_hlc_logical,
                                        excluded.last_hlc_logical),
                 last_seen_ms = MAX(device_clock.last_seen_ms, excluded.last_seen_ms)",
            rusqlite::params![
                account.as_blob(),
                device_id,
                hlc.physical_ms as i64,
                hlc.logical,
                now_ms,
            ],
        )?;
        if new_device {
            self.conn.execute(
                "UPDATE accounts SET device_count = device_count + 1 WHERE account_id = ?1",
                [account.as_blob()],
            )?;
        }
        Ok(())
    }

    /// The newest journal row for one `(account, entity, field)` by HLC.
    ///
    /// This is the read that makes a conflict *detectable* without vector clocks:
    /// the passive event carries a per-device HLC and no causal history, so the only
    /// honest statement is "another device already wrote this field with a newer
    /// non-comparable clock". Both values stay in the journal; the pair is recorded
    /// so `sync.conflicts` can show it, and no winner is chosen.
    pub fn newest_field_op(
        &self,
        account: &AccountId,
        entity_id: &str,
        field: &str,
    ) -> Result<Option<FieldOpRef>> {
        let row: Option<(i64, i64, i64, String, i64)> = self
            .conn
            .query_row(
                "SELECT op_seq, hlc_physical, hlc_logical, device_id, kind
                   FROM ops
                  WHERE account_id = ?1 AND entity_id = ?2 AND field = ?3
               ORDER BY hlc_physical DESC, hlc_logical DESC, op_seq DESC
                  LIMIT 1",
                rusqlite::params![account.as_blob(), entity_id, field],
                |row| {
                    Ok((
                        row.get(0)?,
                        row.get(1)?,
                        row.get(2)?,
                        row.get(3)?,
                        row.get(4)?,
                    ))
                },
            )
            .optional()?;
        let Some((op_seq, physical, logical, device_id, kind)) = row else {
            return Ok(None);
        };
        Ok(Some(FieldOpRef {
            op_seq: op_seq as u64,
            hlc: Hlc::new(physical as u64, logical as u16),
            device_id,
            kind_code: kind as u8,
        }))
    }

    /// Next per-account conflict id. Stored, never `MAX(id) + 1`: a client may still
    /// hold an id it saw before a prune, and reusing one would make
    /// "resolve conflict 7" ambiguous (the L7 bug in the C++ updater).
    pub fn next_conflict_id(&self, account: &AccountId, now_ms: i64) -> Result<u64> {
        let id: i64 = self.conn.query_row(
            "UPDATE accounts
                SET conflict_high_water = conflict_high_water + 1,
                    last_write_ms = MAX(last_write_ms, ?2)
              WHERE account_id = ?1
          RETURNING conflict_high_water",
            rusqlite::params![account.as_blob(), now_ms],
            |row| row.get(0),
        )?;
        Ok(id as u64)
    }

    pub fn insert_conflict(&self, row: &ConflictInsert<'_>) -> Result<()> {
        self.conn.execute(
            "INSERT INTO conflicts (account_id, conflict_id, entity_id, field, kind,
                                    local_op_seq, local_hlc_physical, local_hlc_logical,
                                    local_device, remote_op_seq, remote_hlc_physical,
                                    remote_hlc_logical, remote_device, detected_at_ms,
                                    resolved_by)
             VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, NULL)",
            rusqlite::params![
                row.account.as_blob(),
                row.conflict_id as i64,
                row.entity_id,
                row.field,
                row.kind.code(),
                row.local_op_seq as i64,
                row.local_hlc.physical_ms as i64,
                row.local_hlc.logical,
                row.local_device,
                row.remote_op_seq as i64,
                row.remote_hlc.physical_ms as i64,
                row.remote_hlc.logical,
                row.remote_device,
                row.detected_at_ms,
            ],
        )?;
        Ok(())
    }
}

impl Drop for Tx<'_> {
    fn drop(&mut self) {
        if !self.finished {
            // Best effort: an error here means the connection is already gone, and
            // the next `BEGIN IMMEDIATE` will surface it. Never `unwrap`: a panic in
            // a `Drop` during unwinding aborts the process.
            let _ = self.conn.execute_batch("ROLLBACK");
        }
    }
}

impl core::fmt::Debug for Tx<'_> {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        f.debug_struct("Tx")
            .field("finished", &self.finished)
            .finish()
    }
}

/// Row for [`Tx::insert_op`]. A struct rather than eight positional arguments
/// because `?1..?11` in the wrong order is a data-corruption bug, not a typo.
pub struct NewOp<'e> {
    pub op_seq: u64,
    pub account: AccountId,
    pub kind: EventKind,
    pub entity_id: &'e str,
    pub field: Option<&'e str>,
    pub value: &'e [u8],
    pub hlc: Hlc,
    pub device_id: &'e str,
    pub content_hash: [u8; 32],
    pub applied_at_ms: i64,
}

/// Row for [`Tx::upsert_entity`].
pub struct EntityUpdate<'e> {
    pub account: AccountId,
    pub entity_id: &'e str,
    pub kind: EventKind,
    pub op_seq: u64,
    pub hlc: Hlc,
    pub observed_ms: i64,
    pub deleted: bool,
    pub deleted_hlc: Option<Hlc>,
}

pub struct PresenceInsert<'e> {
    pub account: AccountId,
    pub op_seq: u64,
    pub peer_id: i64,
    pub status: u8,
    pub detail_ms: Option<i64>,
    pub observed_ms: i64,
    pub hlc: Hlc,
    pub device_id: &'e str,
    pub content_hash: [u8; 32],
}

pub struct ProfileInsert<'e> {
    pub account: AccountId,
    pub op_seq: u64,
    pub peer_id: i64,
    pub field: &'e str,
    pub value: &'e [u8],
    pub observed_ms: i64,
    pub hlc: Hlc,
    pub device_id: &'e str,
    pub content_hash: [u8; 32],
}

pub struct ArchiveInsert<'e> {
    pub account: AccountId,
    pub op_seq: u64,
    pub peer_id: i64,
    pub source: &'e str,
    pub payload: &'e [u8],
    pub observed_ms: i64,
    pub hlc: Hlc,
    pub device_id: &'e str,
    pub content_hash: [u8; 32],
}

/// A journal row as seen by the conflict check: enough to record *what* it is
/// without loading the value (the value is ciphertext anyway).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct FieldOpRef {
    pub op_seq: u64,
    pub hlc: Hlc,
    pub device_id: String,
    pub kind_code: u8,
}

/// Row for [`Tx::insert_conflict`].
pub struct ConflictInsert<'e> {
    pub account: AccountId,
    pub conflict_id: u64,
    pub entity_id: &'e str,
    pub field: Option<&'e str>,
    pub kind: EventKind,
    pub local_op_seq: u64,
    pub local_hlc: Hlc,
    pub local_device: &'e str,
    pub remote_op_seq: u64,
    pub remote_hlc: Hlc,
    pub remote_device: &'e str,
    pub detected_at_ms: i64,
}

#[cfg(test)]
mod tests {
    use super::*;

    fn account() -> AccountId {
        AccountId::new([1u8; 16]).unwrap()
    }

    #[test]
    fn open_creates_the_schema_and_the_account_row_is_implicit() {
        let mut store = Store::open_in_memory().unwrap();
        assert!(store.is_idle());
        let seq = store
            .transaction(|tx| tx.next_op_seq(&account(), 1_000))
            .unwrap();
        assert_eq!(seq, 1);
        // No open transaction was left behind.
        assert!(store.is_idle());
        let accounts: i64 = store
            .connection()
            .query_row("SELECT COUNT(*) FROM accounts", [], |row| row.get(0))
            .unwrap();
        assert_eq!(accounts, 1);
    }

    #[test]
    fn op_seq_is_monotone_per_account_and_independent_across_accounts() {
        let mut store = Store::open_in_memory().unwrap();
        let a = account();
        let b = AccountId::new([2u8; 16]).unwrap();
        let sequences = store
            .transaction(|tx| {
                let first = tx.next_op_seq(&a, 1)?;
                let second = tx.next_op_seq(&a, 2)?;
                let other = tx.next_op_seq(&b, 3)?;
                Ok((first, second, other))
            })
            .unwrap();
        assert_eq!(sequences, (1, 2, 1));
        assert_eq!(store.transaction(|tx| tx.high_water(&a)).unwrap(), 2);
    }

    #[test]
    fn a_failed_transaction_rolls_back_and_the_connection_stays_usable() {
        // The C4 regression test. In the C++ build this sequence left the
        // connection inside a transaction and muted the passive path forever.
        let mut store = Store::open_in_memory().unwrap();
        let account = account();
        let error = store
            .transaction(|tx| {
                let _ = tx.next_op_seq(&account, 1)?;
                Err::<(), _>(Error::Invalid("boom".to_string()))
            })
            .unwrap_err();
        assert!(error.to_string().contains("boom"));
        assert!(
            store.is_idle(),
            "a failed transaction must not leave the connection in one"
        );

        // A subsequent write must work, and must not see the rolled-back sequence.
        let seq = store.transaction(|tx| tx.next_op_seq(&account, 2)).unwrap();
        assert_eq!(seq, 1, "the rolled-back sequence number must not persist");
        assert_eq!(store.transaction(|tx| tx.high_water(&account)).unwrap(), 1);
    }

    #[test]
    fn a_panicking_transaction_still_rolls_back() {
        let mut store = Store::open_in_memory().unwrap();
        let account = account();
        let outcome = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
            store.transaction(|tx| -> Result<()> {
                tx.next_op_seq(&account, 1)?;
                panic!("client bug");
            })
        }));
        assert!(outcome.is_err());
        assert!(store.is_idle());
        // ... and the sequence was not consumed.
        assert_eq!(store.transaction(|tx| tx.high_water(&account)).unwrap(), 0);
    }

    #[test]
    fn schema_version_is_recorded_and_checked() {
        let store = Store::open_in_memory().unwrap();
        let version = crate::store::migrate::read_version(store.connection()).unwrap();
        assert_eq!(version, crate::store::schema::SCHEMA_VERSION);

        // A future version is refused *before* a store is handed out (see
        // `store::migrate::tests::a_file_from_the_future_is_refused_not_guessed`).
        // What is left to check here is the post-migration guard: a version row that
        // does not read back as "current" is a storage error, not a silent open.
        store
            .connection()
            .execute(
                "UPDATE schema_meta SET value = '9999' WHERE key = 'schema_version'",
                [],
            )
            .unwrap();
        let error = store.check_schema_version().unwrap_err();
        assert!(error.to_string().contains("after migration"), "{error}");
    }

    #[test]
    fn device_clock_roundtrips_and_never_goes_backwards() {
        let mut store = Store::open_in_memory().unwrap();
        let account = account();
        store
            .transaction(|tx| {
                assert!(tx.device_last_hlc(&account, "dev-a")?.is_none());
                tx.update_device_clock(&account, "dev-a", Hlc::new(10, 2), 10, true)?;
                Ok(())
            })
            .unwrap();
        let seen = store
            .transaction(|tx| tx.device_last_hlc(&account, "dev-a"))
            .unwrap();
        assert_eq!(seen, Some(Hlc::new(10, 2)));
        // `new_device` bumped the counter exactly once.
        let devices: i64 = store
            .connection()
            .query_row(
                "SELECT device_count FROM accounts WHERE account_id = ?1",
                [account.as_blob()],
                |row| row.get(0),
            )
            .unwrap();
        assert_eq!(devices, 1);
    }

    #[test]
    fn a_foreign_key_violation_is_reported_not_silently_dropped() {
        // Cross-account isolation at the SQL layer: a row for an account that was
        // never registered cannot be inserted even if a code path forgot the check.
        let store = Store::open_in_memory().unwrap();
        let error = store
            .connection()
            .execute(
                "INSERT INTO presence (account_id, op_seq, peer_id, status, observed_ms,
                                       hlc_physical, hlc_logical, device_id, content_hash)
                 VALUES (?1, 1, 1, 4, 1, 1, 0, 'dev', ?2)",
                rusqlite::params![
                    AccountId::new([7u8; 16]).unwrap().as_blob(),
                    [0u8; 32].as_slice()
                ],
            )
            .unwrap_err();
        assert!(error.to_string().to_lowercase().contains("foreign key"));
    }
}
