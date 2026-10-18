//! Reading the user's shared database (PostgreSQL / MySQL) and publishing what it
//! contains into the local journal.
//!
//! # Why this module exists
//!
//! `docs/transport.md` § 1.3 fixes the direction of the sync: the agent stands next
//! to the user's database, reads the changes, seals them with the account's DEK and
//! appends them to `ops`; the client only pulls. Until this module existed the only
//! writer into `ops` was HTTP `sync.push`, i.e. the *client* was still the source of
//! truth for everything (§ 15 п. 1–3 of the same document).
//!
//! ```text
//! [общая БД: PostgreSQL / MySQL]          ← источник истины
//!         │  SELECT … WHERE key > ? ORDER BY key LIMIT n
//!         ▼
//! [db::ingest]  seal(subkey(DEK, LABEL_SYNC_DATA)) ──► ops (+ db_cursor, one tx)
//!         ├──► sync.pull ──► C++ client
//!         └──► db.status  ──► observability
//! ```
//!
//! # Why synchronous drivers, and which ones
//!
//! `postgres` (the blocking wrapper over `tokio-postgres`) and `mysql`. The agent's
//! transport is deliberately blocking — one thread per connection, no runtime — so
//! "async driver + `block_on` on a thread" would buy nothing and would add a reactor
//! every request path had to stay out of. The ingest itself is a single periodic
//! thread, which is the natural shape for a blocking client.
//!
//! Both are driven through rustls with `ring`, the same choice as the listener, so
//! no OpenSSL/SChannel enters the musl-static CI path (see `Cargo.toml`).
//!
//! # The two hard boundaries of this module
//!
//! 1. **Never read secrets.** The tables and columns the agent may name are an
//!    explicit allowlist ([`allowlist`]), and `personality_core`, `openai_api_key`,
//!    `pgConnString`, `ssh*` are refused by name as well as by omission
//!    (`docs/transport.md` § 2.6).
//! 2. **Never leak the connection.** The connection string lives in the vault,
//!    sealed under `subkey(DEK, LABEL_DB_CONN)` ([`credentials`]); what leaves the
//!    process is [`ConnFacts`] — engine, host, port, database, user, TLS mode — and
//!    nothing else. Errors carry a class ([`class`]), not a driver message, because
//!    driver messages can contain the connection string or a row value.
//!
//! # What is implemented, and what is not
//!
//! The **outbox** shape is implemented: a table with a monotone key and the columns
//! `(id, kind, entity_id, field, observed_ms, value)`. That is also the shape
//! `docs/transport.md` § 2.6 recommends ("Рекомендуемый вариант — outbox-таблица,
//! которую заполняет клиент или триггер"). The other tables listed in § 2.6 are in
//! the allowlist with their columns — a grant can be prepared for them today — but
//! selecting one as the ingest source is refused with `unsupported_table` until a
//! row-to-event mapping exists for it: mapping `messages.text`/`views`/`media` to
//! one event per field is a modelling decision (§ 12.1/§ 12.9 of
//! `docs/client-integration.md`), not a plumbing detail, and guessing it would put
//! the wrong field names into the journal. See `docs/db-ingest.md` § 7.

use serde::Serialize;

use crate::store::event::EventKind;

pub mod allowlist;
pub mod conninfo;
pub mod credentials;
pub mod ingest;
/// Test double for [`DbSource`]. Compiled only for tests: a release build has no
/// way to answer a `SELECT` other than through the two real drivers.
#[cfg(test)]
pub mod mock;
pub mod mysql;
pub mod postgres;

pub use allowlist::{TableSpec, ALLOWLIST};
pub use conninfo::{ConnFacts, SslMode};
pub use ingest::{DbIngest, DbIngestConfig, IngestHealth, PassReport};
#[cfg(test)]
pub use mock::MockSource;

/// `device_id` of every row the *server* read from the DB.
///
/// Its own device on purpose (`docs/transport.md` § 7, § 15 п. 10): HLC monotonicity
/// is per `(account, device)`, so the server's clock must not be compared with a
/// client device's, and the client needs to tell server-sourced rows from its own.
pub const SERVER_DEVICE_ID: &str = "server-db";

/// Default table the ingest reads, and the default cursor source.
pub const DEFAULT_TABLE: &str = "sync_outbox";

/// Default page size of one `SELECT`. One page must fit `MAX_BATCH_EVENTS` with
/// room for the events one row expands to; the outbox rows are 1:1 today, but the
/// bound is stated in terms of the journal limit rather than of the current mapping.
pub const DEFAULT_BATCH_LIMIT: usize = 200;

/// Hard ceiling on one `SELECT`, checked before the query.
pub const MAX_BATCH_LIMIT: usize = crate::store::MAX_BATCH_EVENTS;

/// Stable error classes.
///
/// These strings are the only thing a DB failure ever contributes to the log, to
/// `db.status.last_error`, and to an error returned over the wire. A class is
/// deliberately *coarse*: "connect" is actionable, `password authentication failed
/// for user "teleforge_agent"` is both less useful in a status field and a leak.
pub mod class {
    /// The vault is locked, so the connection string cannot be unsealed (and the
    /// rows could not be encrypted anyway).
    pub const VAULT_LOCKED: &str = "vault_locked";
    /// The vault is unlocked but holds no connection string for this tenant.
    pub const CREDENTIALS_MISSING: &str = "credentials_missing";
    /// The stored connection string cannot be parsed or names an unknown engine.
    pub const CONFIG: &str = "config";
    /// `sslmode=disable`/`trust`: refused rather than downgraded (§ 2.6 п. 6).
    pub const INSECURE_TRANSPORT: &str = "insecure_transport";
    /// TCP/TLS handshake failed, or the server is not reachable.
    pub const CONNECT: &str = "connect";
    /// The server rejected the credentials.
    pub const AUTH: &str = "auth";
    /// A `SELECT` failed (permissions, syntax, a dropped table).
    pub const QUERY: &str = "query";
    /// A row could not be decoded into an event.
    pub const DECODE: &str = "decode";
    /// The configured table is not in the allowlist.
    pub const TABLE_NOT_ALLOWED: &str = "table_not_allowed";
    /// The table is allowlisted but this build has no row mapping for it.
    pub const UNSUPPORTED_TABLE: &str = "unsupported_table";
    /// Sealing a value failed (wrong key length, AEAD error).
    pub const SEAL: &str = "seal";
    /// The local journal refused the batch.
    pub const STORE: &str = "store";
    /// One page would exceed `MAX_BATCH_EVENTS`.
    pub const TOO_MANY_EVENTS: &str = "too_many_events";
    /// The calling token is not allowed to run the ingest (§ 15 п. 11).
    pub const FORBIDDEN_ROLE: &str = "forbidden_role";
}

/// Map an error to the class reported by `db.status`.
///
/// Only [`crate::error::Error::Database`] carries a class of its own; everything
/// else is either ours (`store`) or the vault's. Keep this total and infallible: it
/// is called from the ingest loop, which must never fail while reporting a failure.
pub fn class_of(error: &crate::error::Error) -> &'static str {
    match error {
        crate::error::Error::Database(class) => class,
        crate::error::Error::Auth(_) => class::VAULT_LOCKED,
        crate::error::Error::Storage(_) => class::STORE,
        crate::error::Error::Crypto(_) => class::SEAL,
        crate::error::Error::Invalid(_) => class::CONFIG,
        _ => class::STORE,
    }
}

/// Which engine a connection string points at.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Engine {
    Postgres,
    MySql,
}

impl Engine {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Postgres => "postgresql",
            Self::MySql => "mysql",
        }
    }

    /// The default TCP port of the engine, used when the URL omits one.
    pub fn default_port(self) -> u16 {
        match self {
            Self::Postgres => 5432,
            Self::MySql => 3306,
        }
    }
}

/// Whether the tenant DEK is available to this process.
///
/// `locked` is a normal state, not an error: `sync.pull` keeps working (the journal
/// holds ciphertext), `db.status` reports it, and the ingest simply does not run.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum VaultState {
    Locked,
    Unlocked,
}

impl VaultState {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Locked => "locked",
            Self::Unlocked => "unlocked",
        }
    }
}

/// One row as the source presents it: a key for the cursor, a timestamp, and either
/// an event or a row that could not be turned into one.
#[derive(Clone, Debug)]
pub struct DbRow {
    /// The monotone key the cursor advances to (`id`, `op_seq`, …).
    pub key: i64,
    /// The row's own observation time. Used for the HLC physical part and for the
    /// deterministic `content_hash`, never for the cursor.
    pub observed_ms: i64,
    pub body: DbRowBody,
}

/// The payload of one [`DbRow`].
#[derive(Clone, Debug)]
pub enum DbRowBody {
    Event {
        kind: EventKind,
        entity_id: String,
        field: Option<String>,
        /// Cleartext, straight from the user's DB. The ingest seals it before it
        /// reaches `ops`; nothing else in the process ever sees it.
        value: Vec<u8>,
    },
    /// The row exists but cannot be decoded (unknown `kind`, wrong column type, a
    /// value too large for the event class).
    ///
    /// It is still a *read* row: the cursor has to move past it, otherwise one bad
    /// row wedges the ingest forever and every later row is never published. The
    /// ingest counts it and logs the class — never the row.
    Malformed,
}

impl DbRow {
    /// True when the row could not be decoded into an event. The cursor still has to
    /// move past it; the ingest counts it as `skipped`.
    pub fn is_malformed(&self) -> bool {
        matches!(self.body, DbRowBody::Malformed)
    }
}

/// Everything `db.status` publishes about the DB side (§ 5.8 of
/// `docs/transport.md`).
///
/// Field names are the contract. The first twelve are exactly § 5.8; `fails`,
/// `batches`, `applied`, `duplicates`, `last_ok_at` and `skipped` are the health
/// counters of `docs/transport.md` § 9 (the ingest loop) — additive, so a client
/// that only knows § 5.8 keeps working.
#[derive(Clone, Debug, Serialize)]
pub struct DbStatus {
    /// `SELECT 1` succeeded on this connection.
    pub connected: bool,
    /// Round-trip time of the probe that produced this report.
    pub latency_ms: u64,
    /// `postgresql` / `mysql`, or `null` before the first successful connect.
    pub engine: Option<&'static str>,
    pub database: Option<String>,
    pub reader_user: Option<String>,
    /// `verify-full` / `verify-ca` / `require` / `tunnel`.
    pub tls: Option<&'static str>,
    pub cursor: Option<CursorStatus>,
    /// `now - last_ok_at`: how far the *publication* lags behind, not how far the
    /// DB is behind the client.
    pub lag_ms: Option<u64>,
    pub last_read_ms: Option<u64>,
    /// Whether the grants of § 2.6 cover every table this build needs.
    pub tables_ok: bool,
    pub vault: &'static str,
    /// Class of the last failure ([`class`]), `null` when the last pass succeeded.
    pub last_error: Option<&'static str>,
    pub last_ok_at: Option<u64>,
    pub fails: u64,
    pub batches: u64,
    pub applied: u64,
    pub duplicates: u64,
    pub skipped: u64,
}

impl DbStatus {
    /// A report from a process that has no DEK and has never connected.
    pub fn locked() -> Self {
        Self {
            connected: false,
            latency_ms: 0,
            engine: None,
            database: None,
            reader_user: None,
            tls: None,
            cursor: None,
            lag_ms: None,
            last_read_ms: None,
            tables_ok: false,
            vault: VaultState::Locked.as_str(),
            last_error: Some(class::VAULT_LOCKED),
            last_ok_at: None,
            fails: 0,
            batches: 0,
            applied: 0,
            duplicates: 0,
            skipped: 0,
        }
    }

    /// `{"table":…,"column":…,"value":…}` of § 5.8.
    pub fn with_cursor(
        mut self,
        row: Option<&crate::store::DbCursorRow>,
        key_column: &str,
    ) -> Self {
        self.cursor = row.map(|row| CursorStatus {
            table: row.source.clone(),
            column: key_column.to_string(),
            value: row.last_key,
        });
        self
    }
}

/// The `cursor` object of `db.status`.
#[derive(Clone, Debug, Serialize)]
pub struct CursorStatus {
    pub table: String,
    pub column: String,
    pub value: i64,
}

/// A reader of the user's DB.
///
/// Two methods on purpose: this is not a query builder, it is "give me the next page
/// after this key". Everything about *which* table, which columns and how a row
/// becomes an event is fixed by [`allowlist`] and by the concrete driver, so a
/// caller cannot widen the read surface.
///
/// `status(&self)` is infallible — it reports the last known state, including the
/// failure — because `db.status` must answer `200` even when the DB is down.
pub trait DbSource {
    fn status(&self) -> DbStatus;
    fn next_batch(&mut self, cursor: i64, limit: usize) -> crate::error::Result<Vec<DbRow>>;
}

/// An outbox row, for tests in other modules (`api::server` drives the whole cycle
/// over TLS). Test-only: the release build has no way to invent a row.
#[cfg(test)]
pub fn tests_row(key: i64, entity: &str, field: &str, value: &[u8]) -> DbRow {
    DbRow {
        key,
        observed_ms: 1_700_000_000_000 + key,
        body: DbRowBody::Event {
            kind: EventKind::MessageNew,
            entity_id: entity.to_string(),
            field: Some(field.to_string()),
            value: value.to_vec(),
        },
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::error::Error;

    #[test]
    fn engine_names_and_default_ports_are_stable() {
        assert_eq!(Engine::Postgres.as_str(), "postgresql");
        assert_eq!(Engine::MySql.as_str(), "mysql");
        assert_eq!(Engine::Postgres.default_port(), 5432);
        assert_eq!(Engine::MySql.default_port(), 3306);
    }

    #[test]
    fn a_locked_report_says_so_without_claiming_a_connection() {
        let status = DbStatus::locked();
        assert!(!status.connected);
        assert!(!status.tables_ok);
        assert_eq!(status.vault, "locked");
        assert_eq!(status.last_error, Some(class::VAULT_LOCKED));
        // Every field of § 5.8 is present in the JSON, even when it is null: the
        // client switches on the keys, not on "is the key there".
        let json = serde_json::to_value(&status).expect("status is serialisable");
        for field in [
            "connected",
            "latency_ms",
            "engine",
            "database",
            "reader_user",
            "tls",
            "cursor",
            "lag_ms",
            "last_read_ms",
            "tables_ok",
            "vault",
            "last_error",
            // health counters of step 3
            "last_ok_at",
            "fails",
            "batches",
            "applied",
            "duplicates",
            "skipped",
        ] {
            assert!(
                json.get(field).is_some(),
                "db.status must always carry `{field}`"
            );
        }
        assert!(json["last_error"].is_string());
        assert!(json["cursor"].is_null());
    }

    #[test]
    fn the_cursor_object_matches_the_contract() {
        let account = crate::store::AccountId::new([1u8; 16]).unwrap();
        let row = crate::store::DbCursorRow {
            account_id: account,
            source: "sync_outbox".to_string(),
            last_key: 4127,
            updated_at: 1,
        };
        let status = DbStatus::locked().with_cursor(Some(&row), "id");
        let json = serde_json::to_value(&status).unwrap();
        assert_eq!(json["cursor"]["table"], "sync_outbox");
        assert_eq!(json["cursor"]["column"], "id");
        assert_eq!(json["cursor"]["value"], 4127);
    }

    #[test]
    fn error_classes_never_carry_a_driver_message() {
        // The whole point: `db.status.last_error` and the ingest log see a class.
        assert_eq!(class_of(&Error::Database(class::CONNECT)), class::CONNECT);
        assert_eq!(
            class_of(&Error::Auth("nope".to_string())),
            class::VAULT_LOCKED
        );
        assert_eq!(class_of(&Error::Storage("disk".to_string())), class::STORE);
        assert_eq!(class_of(&Error::Crypto("bad key".to_string())), class::SEAL);
        // An error that carries a message displays the message, but the *class*
        // reported to the client never does.
        let displayed = Error::Database(class::QUERY).to_string();
        assert_eq!(displayed, "database error: query");
    }

    #[test]
    fn a_row_reports_whether_it_is_malformed() {
        let malformed = DbRow {
            key: 1,
            observed_ms: 1_000,
            body: DbRowBody::Malformed,
        };
        assert!(malformed.is_malformed());
        let event = DbRow {
            key: 2,
            observed_ms: 1_000,
            body: DbRowBody::Event {
                kind: EventKind::MessageNew,
                entity_id: "message:1:2".to_string(),
                field: Some("text".to_string()),
                value: b"hi".to_vec(),
            },
        };
        assert!(!event.is_malformed());
        match &event.body {
            DbRowBody::Event { entity_id, .. } => assert_eq!(entity_id, "message:1:2"),
            other => panic!("unexpected body {other:?}"),
        }
    }
}
