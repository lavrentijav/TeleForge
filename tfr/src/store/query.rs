//! Read side of the passive store: histories, derived "current" values, and the
//! `self-test` health check.
//!
//! Every query is scoped by `account_id` in its `WHERE` clause. That is not a
//! convention: the schema test asserts the column exists on every table, and the
//! functions here have no variant that omits it.
//!
//! # No "current value" column, on purpose
//!
//! `MergeDataDb` kept one row per entity and replaced it, so a second edit in the
//! same second lost (H7). Here the journal is the truth and "current" is a query:
//! [`profile_current`] and [`presence_latest`] are derived from the newest
//! `op_seq`, and every older value stays readable through
//! [`profile_history`]/[`presence_history`].

use rusqlite::{OptionalExtension, Row};

use crate::error::Result;
use crate::store::conn::Store;
use crate::store::event::{AccountId, EventKind};
use crate::store::schema::{REQUIRED_INDEXES, REQUIRED_TABLES, SCHEMA_VERSION};
use crate::sync::clock::Hlc;

/// One journal row.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct StoredOp {
    pub account_id: AccountId,
    pub op_seq: u64,
    pub kind: EventKind,
    pub entity_id: String,
    pub field: Option<String>,
    pub value: Vec<u8>,
    pub hlc: Hlc,
    pub device_id: String,
    pub content_hash: [u8; 32],
    pub applied_at_ms: i64,
}

/// The monotone aggregates of an entity. Never a value.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct EntityState {
    pub entity_id: String,
    pub kind: EventKind,
    pub first_seen_ms: i64,
    pub last_seen_ms: i64,
    pub revision_count: u64,
    pub last_op_seq: u64,
    pub last_hlc: Hlc,
    pub deleted: bool,
    pub deleted_hlc: Option<Hlc>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PresenceRow {
    pub peer_id: i64,
    pub status: u8,
    pub detail_ms: Option<i64>,
    pub observed_ms: i64,
    pub hlc: Hlc,
    pub op_seq: u64,
    pub device_id: String,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ProfileRow {
    pub peer_id: i64,
    pub field: String,
    pub value: Vec<u8>,
    pub observed_ms: i64,
    pub hlc: Hlc,
    pub op_seq: u64,
    pub device_id: String,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ArchiveRow {
    pub peer_id: i64,
    pub source: String,
    pub payload: Vec<u8>,
    pub observed_ms: i64,
    pub hlc: Hlc,
    pub op_seq: u64,
    pub device_id: String,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct AccountInfo {
    pub account_id: AccountId,
    pub created_at_ms: i64,
    pub last_write_ms: i64,
    pub high_water: u64,
    pub device_count: u64,
}

/// A cheap "how far behind am I" answer, the replacement for the full-scan
/// `sha256` of `PgFetchRemoteStamp`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct AccountStamp {
    pub high_water: u64,
    pub last_write_ms: i64,
    pub op_count: u64,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub struct Totals {
    pub accounts: u64,
    pub entities: u64,
    pub ops: u64,
    pub presence: u64,
    pub profiles: u64,
    pub archive: u64,
}

fn count(store: &Store, sql: &str, params: &[&dyn rusqlite::ToSql]) -> Result<u64> {
    let value: i64 = store
        .connection()
        .query_row(sql, params, |row| row.get(0))?;
    Ok(value as u64)
}

fn account_param(account: &AccountId) -> &[u8] {
    account.as_blob()
}

pub fn op_count(store: &Store, account: &AccountId) -> Result<u64> {
    count(
        store,
        "SELECT COUNT(*) FROM ops WHERE account_id = ?1",
        &[&account_param(account)],
    )
}

/// Page of ops after `since_seq`, in `op_seq` order. `limit` is clamped to the
/// same ceiling `ingest` enforces.
pub fn ops_since(
    store: &Store,
    account: &AccountId,
    since_seq: u64,
    limit: u32,
) -> Result<Vec<StoredOp>> {
    let limit = limit.clamp(1, crate::store::ingest::MAX_BATCH_EVENTS as u32);
    let mut statement = store.connection().prepare(
        "SELECT account_id, op_seq, kind, entity_id, field, value, hlc_physical,
                hlc_logical, device_id, content_hash, applied_at_ms
           FROM ops
          WHERE account_id = ?1 AND op_seq > ?2
       ORDER BY op_seq ASC
          LIMIT ?3",
    )?;
    let rows = statement.query_map(
        rusqlite::params![account_param(account), since_seq as i64, limit as i64],
        map_op,
    )?;
    rows.collect::<std::result::Result<Vec<_>, _>>()
        .map_err(Into::into)
}

/// Full history of one entity, oldest first. This is the query that proves there
/// is no last-write-wins: an edited message has as many rows as it had revisions.
pub fn entity_ops(store: &Store, account: &AccountId, entity_id: &str) -> Result<Vec<StoredOp>> {
    let mut statement = store.connection().prepare(
        "SELECT account_id, op_seq, kind, entity_id, field, value, hlc_physical,
                hlc_logical, device_id, content_hash, applied_at_ms
           FROM ops
          WHERE account_id = ?1 AND entity_id = ?2
       ORDER BY op_seq ASC",
    )?;
    let rows = statement.query_map(rusqlite::params![account_param(account), entity_id], map_op)?;
    rows.collect::<std::result::Result<Vec<_>, _>>()
        .map_err(Into::into)
}

pub fn entity_state(
    store: &Store,
    account: &AccountId,
    entity_id: &str,
) -> Result<Option<EntityState>> {
    let state = store
        .connection()
        .query_row(
            "SELECT entity_id, kind, first_seen_ms, last_seen_ms, revision_count,
                    last_op_seq, last_hlc_physical, last_hlc_logical, deleted,
                    deleted_hlc_physical, deleted_hlc_logical
               FROM entities WHERE account_id = ?1 AND entity_id = ?2",
            rusqlite::params![account_param(account), entity_id],
            |row| {
                Ok(EntityState {
                    entity_id: row.get(0)?,
                    kind: kind_at(row, 1)?,
                    first_seen_ms: row.get(2)?,
                    last_seen_ms: row.get(3)?,
                    revision_count: row.get::<_, i64>(4)? as u64,
                    last_op_seq: row.get::<_, i64>(5)? as u64,
                    last_hlc: Hlc::new(row.get::<_, i64>(6)? as u64, row.get(7)?),
                    deleted: row.get::<_, i64>(8)? != 0,
                    deleted_hlc: optional_hlc(row.get(9)?, row.get(10)?),
                })
            },
        )
        .optional()?;
    Ok(state)
}

/// Entities of one kind, most recently observed first. Used by the archive and by
/// the client's "what changed" view.
pub fn entities_by_kind(
    store: &Store,
    account: &AccountId,
    kind: EventKind,
    limit: u32,
) -> Result<Vec<EntityState>> {
    let mut statement = store.connection().prepare(
        "SELECT entity_id, kind, first_seen_ms, last_seen_ms, revision_count,
                last_op_seq, last_hlc_physical, last_hlc_logical, deleted,
                deleted_hlc_physical, deleted_hlc_logical
           FROM entities
          WHERE account_id = ?1 AND kind = ?2
       ORDER BY last_seen_ms DESC, entity_id ASC
          LIMIT ?3",
    )?;
    let rows = statement.query_map(
        rusqlite::params![account_param(account), kind.code(), limit as i64],
        |row| {
            Ok(EntityState {
                entity_id: row.get(0)?,
                kind: kind_at(row, 1)?,
                first_seen_ms: row.get(2)?,
                last_seen_ms: row.get(3)?,
                revision_count: row.get::<_, i64>(4)? as u64,
                last_op_seq: row.get::<_, i64>(5)? as u64,
                last_hlc: Hlc::new(row.get::<_, i64>(6)? as u64, row.get(7)?),
                deleted: row.get::<_, i64>(8)? != 0,
                deleted_hlc: optional_hlc(row.get(9)?, row.get(10)?),
            })
        },
    )?;
    rows.collect::<std::result::Result<Vec<_>, _>>()
        .map_err(Into::into)
}

/// Presence transitions for one peer, newest first.
pub fn presence_history(
    store: &Store,
    account: &AccountId,
    peer_id: i64,
    limit: u32,
) -> Result<Vec<PresenceRow>> {
    let mut statement = store.connection().prepare(
        "SELECT peer_id, status, detail_ms, observed_ms, hlc_physical, hlc_logical,
                op_seq, device_id
           FROM presence
          WHERE account_id = ?1 AND peer_id = ?2
       ORDER BY observed_ms DESC, op_seq DESC
          LIMIT ?3",
    )?;
    let rows = statement.query_map(
        rusqlite::params![account_param(account), peer_id, limit as i64],
        |row| {
            Ok(PresenceRow {
                peer_id: row.get(0)?,
                status: row.get(1)?,
                detail_ms: row.get(2)?,
                observed_ms: row.get(3)?,
                hlc: Hlc::new(row.get::<_, i64>(4)? as u64, row.get(5)?),
                op_seq: row.get::<_, i64>(6)? as u64,
                device_id: row.get(7)?,
            })
        },
    )?;
    rows.collect::<std::result::Result<Vec<_>, _>>()
        .map_err(Into::into)
}

/// Newest presence observation for one peer.
pub fn presence_latest(
    store: &Store,
    account: &AccountId,
    peer_id: i64,
) -> Result<Option<PresenceRow>> {
    Ok(presence_history(store, account, peer_id, 1)?
        .into_iter()
        .next())
}

/// Every observation of one field, newest first. Nothing is ever overwritten.
pub fn profile_history(
    store: &Store,
    account: &AccountId,
    peer_id: i64,
    field: &str,
) -> Result<Vec<ProfileRow>> {
    let mut statement = store.connection().prepare(
        "SELECT peer_id, field, value, observed_ms, hlc_physical, hlc_logical, op_seq,
                device_id
           FROM profiles
          WHERE account_id = ?1 AND peer_id = ?2 AND field = ?3
       ORDER BY observed_ms DESC, op_seq DESC",
    )?;
    let rows = statement.query_map(
        rusqlite::params![account_param(account), peer_id, field],
        |row| {
            Ok(ProfileRow {
                peer_id: row.get(0)?,
                field: row.get(1)?,
                value: row.get(2)?,
                observed_ms: row.get(3)?,
                hlc: Hlc::new(row.get::<_, i64>(4)? as u64, row.get(5)?),
                op_seq: row.get::<_, i64>(6)? as u64,
                device_id: row.get(7)?,
            })
        },
    )?;
    rows.collect::<std::result::Result<Vec<_>, _>>()
        .map_err(Into::into)
}

/// Derived "current" profile: the newest `op_seq` per field.
///
/// This is what replaces the single mutable `PeerArchiveProfile` row. It is a
/// query, so a concurrent edit on another device cannot be lost by a replace —
/// both values are in `profiles`, and the newest wins only for *display*.
pub fn profile_current(
    store: &Store,
    account: &AccountId,
    peer_id: i64,
) -> Result<Vec<ProfileRow>> {
    let mut statement = store.connection().prepare(
        "SELECT peer_id, field, value, observed_ms, hlc_physical, hlc_logical, op_seq,
                device_id
           FROM profiles AS p
          WHERE account_id = ?1 AND peer_id = ?2
            AND op_seq = (SELECT MAX(q.op_seq) FROM profiles AS q
                           WHERE q.account_id = p.account_id
                             AND q.peer_id = p.peer_id
                             AND q.field = p.field)
       ORDER BY field ASC",
    )?;
    let rows = statement.query_map(rusqlite::params![account_param(account), peer_id], |row| {
        Ok(ProfileRow {
            peer_id: row.get(0)?,
            field: row.get(1)?,
            value: row.get(2)?,
            observed_ms: row.get(3)?,
            hlc: Hlc::new(row.get::<_, i64>(4)? as u64, row.get(5)?),
            op_seq: row.get::<_, i64>(6)? as u64,
            device_id: row.get(7)?,
        })
    })?;
    rows.collect::<std::result::Result<Vec<_>, _>>()
        .map_err(Into::into)
}

/// Archive observations for one peer, newest first.
pub fn archive_observations(
    store: &Store,
    account: &AccountId,
    peer_id: i64,
) -> Result<Vec<ArchiveRow>> {
    let mut statement = store.connection().prepare(
        "SELECT peer_id, source, payload, observed_ms, hlc_physical, hlc_logical,
                op_seq, device_id
           FROM archive_obs
          WHERE account_id = ?1 AND peer_id = ?2
       ORDER BY observed_ms DESC, op_seq DESC",
    )?;
    let rows = statement.query_map(rusqlite::params![account_param(account), peer_id], |row| {
        Ok(ArchiveRow {
            peer_id: row.get(0)?,
            source: row.get(1)?,
            payload: row.get(2)?,
            observed_ms: row.get(3)?,
            hlc: Hlc::new(row.get::<_, i64>(4)? as u64, row.get(5)?),
            op_seq: row.get::<_, i64>(6)? as u64,
            device_id: row.get(7)?,
        })
    })?;
    rows.collect::<std::result::Result<Vec<_>, _>>()
        .map_err(Into::into)
}

pub fn accounts(store: &Store) -> Result<Vec<AccountInfo>> {
    let mut statement = store.connection().prepare(
        "SELECT account_id, created_at_ms, last_write_ms, high_water, device_count
           FROM accounts ORDER BY created_at_ms ASC, account_id ASC",
    )?;
    let rows = statement.query_map([], |row| {
        Ok(AccountInfo {
            account_id: AccountId::from_slice(&row.get::<_, Vec<u8>>(0)?).map_err(|error| {
                rusqlite::Error::FromSqlConversionFailure(
                    0,
                    rusqlite::types::Type::Blob,
                    Box::new(error),
                )
            })?,
            created_at_ms: row.get(1)?,
            last_write_ms: row.get(2)?,
            high_water: row.get::<_, i64>(3)? as u64,
            device_count: row.get::<_, i64>(4)? as u64,
        })
    })?;
    rows.collect::<std::result::Result<Vec<_>, _>>()
        .map_err(Into::into)
}

pub fn account_stamp(store: &Store, account: &AccountId) -> Result<AccountStamp> {
    let row = store
        .connection()
        .query_row(
            "SELECT high_water, last_write_ms FROM accounts WHERE account_id = ?1",
            [account_param(account)],
            |row| Ok((row.get::<_, i64>(0)?, row.get::<_, i64>(1)?)),
        )
        .optional()?;
    let (high_water, last_write_ms) = row.unwrap_or((0, 0));
    Ok(AccountStamp {
        high_water: high_water as u64,
        last_write_ms,
        op_count: op_count(store, account)?,
    })
}

pub fn totals(store: &Store) -> Result<Totals> {
    Ok(Totals {
        accounts: count(store, "SELECT COUNT(*) FROM accounts", &[])?,
        entities: count(store, "SELECT COUNT(*) FROM entities", &[])?,
        ops: count(store, "SELECT COUNT(*) FROM ops", &[])?,
        presence: count(store, "SELECT COUNT(*) FROM presence", &[])?,
        profiles: count(store, "SELECT COUNT(*) FROM profiles", &[])?,
        archive: count(store, "SELECT COUNT(*) FROM archive_obs", &[])?,
    })
}

fn map_op(row: &Row<'_>) -> rusqlite::Result<StoredOp> {
    let account_bytes: Vec<u8> = row.get(0)?;
    Ok(StoredOp {
        account_id: AccountId::from_slice(&account_bytes).map_err(|error| {
            rusqlite::Error::FromSqlConversionFailure(
                0,
                rusqlite::types::Type::Blob,
                Box::new(error),
            )
        })?,
        op_seq: row.get::<_, i64>(1)? as u64,
        kind: kind_at(row, 2)?,
        entity_id: row.get(3)?,
        field: row.get(4)?,
        value: row.get(5)?,
        hlc: Hlc::new(row.get::<_, i64>(6)? as u64, row.get(7)?),
        device_id: row.get(8)?,
        content_hash: hash_at(row, 9)?,
        applied_at_ms: row.get(10)?,
    })
}

fn kind_at(row: &Row<'_>, index: usize) -> rusqlite::Result<EventKind> {
    let code: u8 = row.get(index)?;
    EventKind::from_code(code).ok_or_else(|| {
        rusqlite::Error::FromSqlConversionFailure(
            index,
            rusqlite::types::Type::Integer,
            Box::new(crate::error::Error::Storage(format!(
                "unknown event kind {code} in the store"
            ))),
        )
    })
}

fn hash_at(row: &Row<'_>, index: usize) -> rusqlite::Result<[u8; 32]> {
    let bytes: Vec<u8> = row.get(index)?;
    if bytes.len() != 32 {
        return Err(rusqlite::Error::FromSqlConversionFailure(
            index,
            rusqlite::types::Type::Blob,
            Box::new(crate::error::Error::Storage(format!(
                "content_hash must be 32 bytes, got {}",
                bytes.len()
            ))),
        ));
    }
    let mut out = [0u8; 32];
    out.copy_from_slice(&bytes);
    Ok(out)
}

fn optional_hlc(physical: Option<i64>, logical: Option<u16>) -> Option<Hlc> {
    match (physical, logical) {
        (Some(physical), Some(logical)) => Some(Hlc::new(physical as u64, logical)),
        _ => None,
    }
}

/// One conflict row as `sync.conflicts` reports it.
///
/// Both sides are kept in the journal; this row is the *index* over them. Nothing
/// in the agent chooses between them: `resolved_by` stays `NULL` until a human, via
/// the client, says which value they want. That is the H7 fix at the API level.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ConflictRow {
    pub conflict_id: u64,
    pub entity_id: String,
    pub field: Option<String>,
    pub kind: EventKind,
    pub local_op_seq: u64,
    pub local_hlc: Hlc,
    pub local_device: String,
    pub remote_op_seq: u64,
    pub remote_hlc: Hlc,
    pub remote_device: String,
    pub detected_at_ms: i64,
    pub resolved_by: Option<String>,
}

impl ConflictRow {
    pub fn is_resolved(&self) -> bool {
        self.resolved_by.is_some()
    }

    pub fn to_json(&self) -> String {
        let escape = crate::api::protocol::json_escape;
        format!(
            concat!(
                "{{\"conflict_id\":{},\"entity_id\":\"{}\",\"field\":{},\"kind\":{},",
                "\"local_op_seq\":{},\"local_hlc\":\"{}\",\"local_device\":\"{}\",",
                "\"remote_op_seq\":{},\"remote_hlc\":\"{}\",\"remote_device\":\"{}\",",
                "\"detected_at_ms\":{},\"resolved_by\":{}}}"
            ),
            self.conflict_id,
            escape(&self.entity_id),
            match &self.field {
                Some(field) => format!("\"{}\"", escape(field)),
                None => "null".to_string(),
            },
            self.kind.code(),
            self.local_op_seq,
            self.local_hlc.to_wire(),
            escape(&self.local_device),
            self.remote_op_seq,
            self.remote_hlc.to_wire(),
            escape(&self.remote_device),
            self.detected_at_ms,
            match &self.resolved_by {
                Some(who) => format!("\"{}\"", escape(who)),
                None => "null".to_string(),
            },
        )
    }
}

/// Conflicts with `conflict_id > since_id`, oldest first.
///
/// Paged by the same ceiling as `ops_since`: one request cannot ask the agent to
/// materialise an unbounded list. Resolved conflicts are included by default
/// because a client that reconnects needs to see "you already answered this one"
/// rather than have it vanish mid-list.
pub fn conflict_list(
    store: &Store,
    account: &AccountId,
    since_id: u64,
    limit: u32,
    include_resolved: bool,
) -> Result<Vec<ConflictRow>> {
    let limit = limit.clamp(1, crate::store::ingest::MAX_BATCH_EVENTS as u32);
    let mut statement = store.connection().prepare(
        "SELECT conflict_id, entity_id, field, kind, local_op_seq,
                local_hlc_physical, local_hlc_logical, local_device,
                remote_op_seq, remote_hlc_physical, remote_hlc_logical, remote_device,
                detected_at_ms, resolved_by
           FROM conflicts
          WHERE account_id = ?1
            AND conflict_id > ?2
            AND (?3 = 1 OR resolved_by IS NULL)
       ORDER BY conflict_id ASC
          LIMIT ?4",
    )?;
    let rows = statement.query_map(
        rusqlite::params![
            account_param(account),
            since_id as i64,
            i64::from(include_resolved),
            limit as i64
        ],
        |row| {
            let field: Option<String> = row.get(2)?;
            Ok(ConflictRow {
                conflict_id: row.get::<_, i64>(0)? as u64,
                entity_id: row.get(1)?,
                field,
                kind: kind_at(row, 3)?,
                local_op_seq: row.get::<_, i64>(4)? as u64,
                local_hlc: Hlc::new(row.get::<_, i64>(5)? as u64, row.get(6)?),
                local_device: row.get(7)?,
                remote_op_seq: row.get::<_, i64>(8)? as u64,
                remote_hlc: Hlc::new(row.get::<_, i64>(9)? as u64, row.get(10)?),
                remote_device: row.get(11)?,
                detected_at_ms: row.get(12)?,
                resolved_by: row.get(13)?,
            })
        },
    )?;
    rows.collect::<std::result::Result<Vec<_>, _>>()
        .map_err(Into::into)
}

/// Mark a conflict resolved by `resolved_by` (a device id or a user-visible note).
///
/// Returns `false` when the id does not exist, so the API can answer "no such
/// conflict" instead of pretending. Re-resolving an already resolved conflict is
/// allowed and overwrites the marker: the last human decision is the one that
/// counts, and there is nothing to lose because both values stay in the journal.
pub fn resolve_conflict(
    store: &mut Store,
    account: &AccountId,
    conflict_id: u64,
    resolved_by: &str,
) -> Result<bool> {
    store.transaction(|tx| {
        let changed = tx.connection().execute(
            "UPDATE conflicts SET resolved_by = ?1
              WHERE account_id = ?2 AND conflict_id = ?3",
            rusqlite::params![resolved_by, account.as_blob(), conflict_id as i64],
        )?;
        Ok(changed > 0)
    })
}

/// The report `self-test` prints. `problems()` is empty for a healthy store.
#[derive(Clone, Debug)]
pub struct SelfCheck {
    pub path: String,
    pub schema_version: Option<String>,
    pub journal_mode: String,
    pub synchronous: i64,
    pub busy_timeout_ms: i64,
    pub tables: Vec<String>,
    pub indexes: Vec<String>,
}

impl SelfCheck {
    /// Every way the store can be wrong, as human-readable lines. Empty means OK.
    pub fn problems(&self) -> Vec<String> {
        let mut problems = Vec::new();
        match &self.schema_version {
            Some(version) if version == &SCHEMA_VERSION.to_string() => {}
            Some(version) => problems.push(format!(
                "schema_version {version} in the store, this agent writes {SCHEMA_VERSION}"
            )),
            None => problems.push("schema_meta has no schema_version row".to_string()),
        }
        if self.journal_mode != "wal" {
            problems.push(format!(
                "journal_mode is {:?}, expected \"wal\" (WAL is what keeps a reader from blocking ingest)",
                self.journal_mode
            ));
        }
        // 1 = NORMAL, 2 = FULL. Both are acceptable on disk; 0 (OFF) is not.
        if !matches!(self.synchronous, 1 | 2) {
            problems.push(format!(
                "synchronous is {}, expected 1 (NORMAL) or 2 (FULL)",
                self.synchronous
            ));
        }
        if self.busy_timeout_ms <= 0 {
            problems.push(format!(
                "busy_timeout is {}, which reintroduces the H6 SQLITE_BUSY failure",
                self.busy_timeout_ms
            ));
        }
        for table in REQUIRED_TABLES {
            if !self.tables.iter().any(|name| name == table) {
                problems.push(format!("table {table} is missing"));
            }
        }
        for index in REQUIRED_INDEXES {
            if !self.indexes.iter().any(|name| name == index) {
                problems.push(format!("index {index} is missing"));
            }
        }
        problems
    }

    pub fn ok(&self) -> bool {
        self.problems().is_empty()
    }

    pub fn to_json(&self) -> String {
        let escape = crate::api::protocol::json_escape;
        format!(
            concat!(
                "{{\"path\":\"{}\",\"schema_version\":{},\"journal_mode\":\"{}\",",
                "\"synchronous\":{},\"busy_timeout_ms\":{},\"ok\":{},\"problems\":[{}]}}"
            ),
            escape(&self.path),
            match &self.schema_version {
                Some(version) => format!("\"{}\"", escape(version)),
                None => "null".to_string(),
            },
            escape(&self.journal_mode),
            self.synchronous,
            self.busy_timeout_ms,
            self.ok(),
            self.problems()
                .iter()
                .map(|problem| format!("\"{}\"", escape(problem)))
                .collect::<Vec<_>>()
                .join(",")
        )
    }
}

/// Inspect an open store. Read-only: `self-test` must not modify an installation
/// it is about to reject.
pub fn self_check(store: &Store) -> Result<SelfCheck> {
    let connection = store.connection();
    let schema_version: Option<String> = connection
        .query_row(
            "SELECT value FROM schema_meta WHERE key = 'schema_version'",
            [],
            |row| row.get(0),
        )
        .optional()?;
    let journal_mode: String = connection.query_row("PRAGMA journal_mode", [], |row| row.get(0))?;
    let synchronous: i64 = connection.query_row("PRAGMA synchronous", [], |row| row.get(0))?;
    let busy_timeout_ms: i64 = connection.query_row("PRAGMA busy_timeout", [], |row| row.get(0))?;

    let mut statement = connection.prepare(
        "SELECT name FROM sqlite_master
          WHERE (type = 'table' OR type = 'index') AND name NOT LIKE 'sqlite_%'
       ORDER BY type ASC, name ASC",
    )?;
    let names: Vec<String> = statement
        .query_map([], |row| row.get::<_, String>(0))?
        .collect::<std::result::Result<Vec<_>, _>>()?;
    let (tables, indexes): (Vec<String>, Vec<String>) = names
        .into_iter()
        .partition(|name| !name.starts_with("idx_"));

    Ok(SelfCheck {
        path: store.path().to_string_lossy().to_string(),
        schema_version,
        journal_mode,
        synchronous,
        busy_timeout_ms,
        tables,
        indexes,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::store::event::{Event, IngestResult};
    use crate::store::ingest::{ingest_batch_at, MAX_BATCH_EVENTS};
    use crate::sync::clock::Hlc;

    fn account() -> AccountId {
        AccountId::new([0x11; 16]).unwrap()
    }

    fn event(
        account: AccountId,
        kind: EventKind,
        entity: &str,
        field: Option<&str>,
        value: &[u8],
        hlc: Hlc,
    ) -> Event {
        Event::new(account, "dev-a", kind, entity, field, value.to_vec(), hlc)
    }

    fn seeded() -> (Store, AccountId) {
        let mut store = Store::open_in_memory().unwrap();
        let account = account();
        let events = vec![
            event(
                account,
                EventKind::ProfileUpdate,
                "profile:900",
                Some("username"),
                b"@old",
                Hlc::new(1_000, 0),
            ),
            event(
                account,
                EventKind::ProfileUpdate,
                "profile:900",
                Some("bio"),
                b"first bio",
                Hlc::new(1_001, 0),
            ),
            event(
                account,
                EventKind::ProfileUpdate,
                "profile:900",
                Some("username"),
                b"@new",
                Hlc::new(1_002, 0),
            ),
            event(
                account,
                EventKind::PresenceOnline,
                "presence:900",
                None,
                &5_000i64.to_be_bytes(),
                Hlc::new(1_003, 0),
            ),
            event(
                account,
                EventKind::ArchiveObservation,
                "archive:900",
                Some("participant"),
                b"chat=1",
                Hlc::new(1_004, 0),
            ),
        ];
        let outcome = ingest_batch_at(&mut store, &account, &events, 9_000).unwrap();
        assert_eq!(outcome.counts(), (5, 0, 0));
        (store, account)
    }

    #[test]
    fn conflicts_are_listed_resolved_and_paged() {
        let mut store = Store::open_in_memory().unwrap();
        let account = account();
        // `dev-a` writes a newer value first, then an older observation arrives from
        // `dev-b`: an HLC-ordered merge would drop it, so it becomes a conflict.
        let events = vec![
            Event::new(
                account,
                "dev-a",
                EventKind::ProfileUpdate,
                "profile:900",
                Some("username"),
                b"@new".to_vec(),
                Hlc::new(2_000, 0),
            ),
            Event::new(
                account,
                "dev-b",
                EventKind::ProfileUpdate,
                "profile:900",
                Some("username"),
                b"@old".to_vec(),
                Hlc::new(1_000, 0),
            ),
            Event::new(
                account,
                "dev-c",
                EventKind::ProfileUpdate,
                "profile:901",
                Some("bio"),
                b"later".to_vec(),
                Hlc::new(3_000, 0),
            ),
            Event::new(
                account,
                "dev-d",
                EventKind::ProfileUpdate,
                "profile:901",
                Some("bio"),
                b"earlier".to_vec(),
                Hlc::new(500, 0),
            ),
        ];
        let outcome = ingest_batch_at(&mut store, &account, &events, 9_000).unwrap();
        assert_eq!(outcome.counts(), (4, 0, 0));
        assert_eq!(outcome.conflicts, 2);
        assert_eq!(outcome.conflict_ids, vec![1, 2]);

        let conflicts = conflict_list(&store, &account, 0, 10, true).unwrap();
        assert_eq!(conflicts.len(), 2);
        assert_eq!(conflicts[0].conflict_id, 1);
        assert_eq!(conflicts[0].entity_id, "profile:900");
        assert_eq!(conflicts[0].field.as_deref(), Some("username"));
        assert_eq!(conflicts[0].local_device, "dev-a");
        assert_eq!(conflicts[0].remote_device, "dev-b");
        assert!(conflicts[0].local_hlc > conflicts[0].remote_hlc);
        assert!(!conflicts[0].is_resolved());

        // Paging by id.
        let page = conflict_list(&store, &account, 1, 10, true).unwrap();
        assert_eq!(page.len(), 1);
        assert_eq!(page[0].conflict_id, 2);

        // Resolving is explicit and idempotent.
        assert!(resolve_conflict(&mut store, &account, 1, "user@desktop").unwrap());
        let resolved = conflict_list(&store, &account, 0, 10, true).unwrap();
        assert_eq!(resolved[0].resolved_by.as_deref(), Some("user@desktop"));
        assert!(resolved[0].is_resolved());
        assert!(
            !resolve_conflict(&mut store, &account, 99, "user@desktop").unwrap(),
            "an unknown id is reported, not invented"
        );

        // Unresolved-only view hides what the human already answered.
        let open = conflict_list(&store, &account, 0, 10, false).unwrap();
        assert_eq!(open.len(), 1);
        assert_eq!(open[0].conflict_id, 2);

        // JSON is stable enough for the client to parse.
        let json = resolved[0].to_json();
        assert!(json.contains("\"conflict_id\":1"));
        assert!(json.contains("\"local_device\":\"dev-a\""));
        assert!(json.contains("\"resolved_by\":\"user@desktop\""));
        assert!(json.contains("\"local_hlc\":\""));

        // Another account sees nothing.
        let other = AccountId::new([0x33; 16]).unwrap();
        assert!(conflict_list(&store, &other, 0, 10, true)
            .unwrap()
            .is_empty());
    }

    #[test]
    fn profile_current_returns_the_newest_value_per_field() {
        let (store, account) = seeded();
        let current = profile_current(&store, &account, 900).unwrap();
        assert_eq!(current.len(), 2, "one row per field, not per observation");
        let username = current
            .iter()
            .find(|row| row.field == "username")
            .expect("username row");
        assert_eq!(username.value, b"@new");
        let bio = current
            .iter()
            .find(|row| row.field == "bio")
            .expect("bio row");
        assert_eq!(bio.value, b"first bio");

        // ... while the history still has both usernames.
        let history = profile_history(&store, &account, 900, "username").unwrap();
        assert_eq!(history.len(), 2);
        assert_eq!(history[0].value, b"@new");
        assert_eq!(history[1].value, b"@old");
    }

    #[test]
    fn ops_pagination_is_stable_and_ordered() {
        let (store, account) = seeded();
        let first = ops_since(&store, &account, 0, 2).unwrap();
        assert_eq!(
            first.iter().map(|op| op.op_seq).collect::<Vec<_>>(),
            vec![1, 2]
        );
        let second = ops_since(&store, &account, 2, 2).unwrap();
        assert_eq!(
            second.iter().map(|op| op.op_seq).collect::<Vec<_>>(),
            vec![3, 4]
        );
        let last = ops_since(&store, &account, 4, 10).unwrap();
        assert_eq!(last.len(), 1);
        assert_eq!(last[0].op_seq, 5);
        assert!(ops_since(&store, &account, 5, 10).unwrap().is_empty());
        // The limit is clamped, not trusted.
        let clamped = ops_since(&store, &account, 0, u32::MAX).unwrap();
        assert!(clamped.len() <= MAX_BATCH_EVENTS);
    }

    #[test]
    fn ops_roundtrip_binary_values_and_hashes() {
        let (store, account) = seeded();
        let ops = entity_ops(&store, &account, "profile:900").unwrap();
        assert_eq!(ops.len(), 3);
        assert_eq!(ops[0].value, b"@old");
        assert_eq!(ops[0].account_id, account);
        assert_eq!(ops[0].device_id, "dev-a");
        assert_ne!(ops[0].content_hash, [0u8; 32]);
        assert_eq!(ops[0].applied_at_ms, 9_000);
        assert_eq!(ops[0].field.as_deref(), Some("username"));
    }

    #[test]
    fn entity_state_aggregates_and_unknown_entities() {
        let (store, account) = seeded();
        let state = entity_state(&store, &account, "profile:900")
            .unwrap()
            .expect("entity present");
        assert_eq!(state.kind, EventKind::ProfileUpdate);
        assert_eq!(state.revision_count, 3);
        assert_eq!(state.last_op_seq, 3);
        assert!(!state.deleted);
        assert_eq!(state.deleted_hlc, None);
        assert!(entity_state(&store, &account, "profile:901")
            .unwrap()
            .is_none());
    }

    #[test]
    fn entities_by_kind_filters_and_orders() {
        let (store, account) = seeded();
        let profiles = entities_by_kind(&store, &account, EventKind::ProfileUpdate, 10).unwrap();
        assert_eq!(profiles.len(), 1);
        assert_eq!(profiles[0].entity_id, "profile:900");
        let presence = entities_by_kind(&store, &account, EventKind::PresenceOnline, 10).unwrap();
        assert_eq!(presence.len(), 1);
        assert!(
            entities_by_kind(&store, &account, EventKind::MessageNew, 10)
                .unwrap()
                .is_empty()
        );
    }

    #[test]
    fn presence_and_archive_reads_are_scoped_to_one_peer() {
        let (store, account) = seeded();
        assert_eq!(
            presence_history(&store, &account, 900, 10).unwrap().len(),
            1
        );
        assert_eq!(
            presence_history(&store, &account, 901, 10).unwrap().len(),
            0
        );
        assert_eq!(
            archive_observations(&store, &account, 900).unwrap().len(),
            1
        );
        assert_eq!(
            archive_observations(&store, &account, 901).unwrap().len(),
            0
        );
        assert_eq!(
            presence_latest(&store, &account, 900)
                .unwrap()
                .unwrap()
                .detail_ms,
            Some(5_000)
        );
        assert!(presence_latest(&store, &account, 901).unwrap().is_none());
    }

    #[test]
    fn accounts_and_stamp_report_the_high_water() {
        let (store, account) = seeded();
        let accounts = accounts(&store).unwrap();
        assert_eq!(accounts.len(), 1);
        assert_eq!(accounts[0].account_id, account);
        assert_eq!(accounts[0].high_water, 5);
        assert_eq!(accounts[0].device_count, 1);

        let stamp = account_stamp(&store, &account).unwrap();
        assert_eq!(stamp.high_water, 5);
        assert_eq!(stamp.op_count, 5);
        assert_eq!(stamp.last_write_ms, 9_000);

        let unknown = account_stamp(&store, &AccountId::new([0x22; 16]).unwrap()).unwrap();
        assert_eq!(stamp.high_water, 5);
        assert_eq!(unknown.high_water, 0);
        assert_eq!(unknown.op_count, 0);
    }

    #[test]
    fn totals_count_every_table() {
        let (store, _account) = seeded();
        let totals = totals(&store).unwrap();
        assert_eq!(totals.accounts, 1);
        assert_eq!(totals.ops, 5);
        assert_eq!(totals.entities, 3);
        assert_eq!(totals.profiles, 3);
        assert_eq!(totals.presence, 1);
        assert_eq!(totals.archive, 1);
    }

    #[test]
    fn self_check_accepts_a_healthy_file_store() {
        let directory = crate::test_support::TestDir::new("query");
        let path = directory.join("data.db");
        let store = Store::open(&path).unwrap();
        let check = self_check(&store).unwrap();
        assert!(check.ok(), "problems: {:?}", check.problems());
        assert_eq!(check.journal_mode, "wal");
        assert_eq!(check.busy_timeout_ms, 5000);
        assert_eq!(check.synchronous, 1);
        assert_eq!(
            check.schema_version.as_deref(),
            Some(SCHEMA_VERSION.to_string().as_str())
        );
        for table in REQUIRED_TABLES {
            assert!(check.tables.iter().any(|name| name == table));
        }
        let json = check.to_json();
        assert!(json.contains("\"ok\":true"));
        assert!(json.contains("\"journal_mode\":\"wal\""));
    }

    #[test]
    fn self_check_reports_a_broken_store_instead_of_panicking() {
        let store = Store::open_in_memory().unwrap();
        let check = self_check(&store).unwrap();
        // In-memory SQLite cannot do WAL, so this is the one problem we expect.
        let problems = check.problems();
        assert!(
            problems
                .iter()
                .any(|problem| problem.contains("journal_mode")),
            "problems: {problems:?}"
        );
        assert!(check.to_json().contains("\"ok\":false"));
    }

    #[test]
    fn self_check_flags_a_downgraded_pragma() {
        let store = Store::open_in_memory().unwrap();
        store
            .connection()
            .execute_batch("PRAGMA busy_timeout = 0")
            .unwrap();
        let check = self_check(&store).unwrap();
        assert_eq!(check.busy_timeout_ms, 0);
        assert!(check
            .problems()
            .iter()
            .any(|problem| problem.contains("busy_timeout")));
    }

    #[test]
    fn self_check_flags_a_missing_index() {
        let store = Store::open_in_memory().unwrap();
        store
            .connection()
            .execute_batch("DROP INDEX idx_ops_applied")
            .unwrap();
        let check = self_check(&store).unwrap();
        assert!(check
            .problems()
            .iter()
            .any(|problem| problem.contains("idx_ops_applied")));
    }

    #[test]
    fn duplicate_verdicts_do_not_grow_the_history() {
        let (mut store, account) = seeded();
        let before = totals(&store).unwrap();
        let event = event(
            account,
            EventKind::ProfileUpdate,
            "profile:900",
            Some("username"),
            b"@old",
            Hlc::new(1_000, 0),
        );
        let outcome =
            ingest_batch_at(&mut store, &account, std::slice::from_ref(&event), 10_000).unwrap();
        assert!(matches!(outcome.results[0], IngestResult::Duplicate { .. }));
        let after = totals(&store).unwrap();
        assert_eq!(before.ops, after.ops);
        assert_eq!(before.profiles, after.profiles);
        assert_eq!(before.entities, after.entities);
    }
}
