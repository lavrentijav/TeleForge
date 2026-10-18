//! The periodic ingest: read the user's DB, seal each row, append it to `ops`.
//!
//! # One pass
//!
//! ```text
//! 1. key material      subkey(DEK, LABEL_SYNC_DATA) — нет DEK → Error::Auth, журнал не трогаем
//! 2. cursor            db_cursor(account, source) → last_key (0 when nothing was read)
//! 3. connect           credentials (vault) → ConnFacts → driver (lazy, once per failure-free run)
//! 4. page              SELECT … WHERE key > last_key ORDER BY key ASC LIMIT n
//! 5. seal              value → AEAD(subkey, aad = address of the row)
//! 6. one transaction   ops rows + db_cursor advance   ← both or neither
//! 7. health            last_ok_at / lag_ms / fails / counters
//! ```
//!
//! # The three invariants this file is responsible for
//!
//! 1. **A failed pass does not move the cursor.** Step 6 is one transaction, so a
//!    failure anywhere leaves the cursor where it was and the next pass re-reads the
//!    same rows. Re-reading cannot duplicate them: the cursor write and the `ops`
//!    rows commit together, and `content_hash` is computed from the *row*
//!    ([`row_content_hash`]) rather than from the journal HLC, so even a batch that
//!    is replayed after a store failure hashes the same and dedupes.
//! 2. **A failed pass does not kill the thread.** [`supervise`] is the only place
//!    that loops, and it logs the *class* of the error, not its message.
//! 3. **Nothing about the DB leaves this module except [`DbStatus`].** The
//!    connection string, the values and the driver messages stay here; what a client
//!    sees is § 5.8 of `docs/transport.md`.
//!
//! # Why the HLC is seeded from the store
//!
//! `device_clock` holds the last HLC per `(account, device)`, and the ingest uses
//! [`super::SERVER_DEVICE_ID`] as its device. Seeding the counter from the store
//! means a restart cannot produce an HLC that the journal has already seen — which
//! `store::ingest` would reject as `non_monotonic_hlc`, blocking the ingest forever.

use std::sync::Mutex;

use crate::crypto::aead::{self, AeadAlgorithm};
use crate::crypto::kdf;
use crate::db::allowlist::{self, TableSpec};
use crate::db::{class, class_of, conninfo, credentials, DbRow, DbRowBody, DbSource, DbStatus};
use crate::db::{DEFAULT_BATCH_LIMIT, DEFAULT_TABLE, MAX_BATCH_LIMIT, SERVER_DEVICE_ID};
use crate::error::{Error, Result};
use crate::logging::Logger;
use crate::secret::SecretBytes;
use crate::store::conn::Store;
use crate::store::event::{AccountId, Event, EventKind};
use crate::sync::clock::{Hlc, MAX_LOGICAL};

/// Largest accepted value from the DB, in bytes. The same ceiling `store::ingest`
/// applies per event class, checked here so an oversized row is refused *before* it
/// is kept in memory twice (clear + sealed).
pub const MAX_ROW_VALUE_BYTES: usize = crate::store::event::MAX_VALUE_BYTES;

/// Connect timeout when the connection string does not name one. Bounded on
/// purpose: `db.status` connects on demand, and a client polling it must not wait
/// for the kernel's two-minute TCP timeout.
pub const DEFAULT_CONNECT_TIMEOUT_SECS: u64 = 5;

/// Everything the ingest needs. **No secret**: the connection string is in the vault
/// (`credentials`), and the DEK is in memory only while the vault is unlocked.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DbIngestConfig {
    pub data_dir: String,
    /// Tenant whose store and vault slot the ingest uses.
    pub tenant: String,
    /// Account whose journal the rows are published into. The DB rows carry the
    /// account implicitly — a DB belongs to one account — so it is configuration,
    /// not a column.
    pub account: AccountId,
    /// `0` disables the scheduler (the functions stay callable, which is what
    /// `db.ingest` uses).
    pub interval_secs: u64,
    /// Allowlisted table to read.
    pub table: String,
    pub batch_limit: usize,
}

impl DbIngestConfig {
    pub fn new(data_dir: impl Into<String>, tenant: impl Into<String>, account: AccountId) -> Self {
        Self {
            data_dir: data_dir.into(),
            tenant: tenant.into(),
            account,
            interval_secs: 0,
            table: DEFAULT_TABLE.to_string(),
            batch_limit: DEFAULT_BATCH_LIMIT,
        }
    }

    /// The `db_cursor.source` value: the table being read.
    pub fn source(&self) -> &str {
        &self.table
    }

    /// Refuse a configuration that would read something the allowlist does not
    /// cover, or a page size that could not fit one batch.
    ///
    /// Returns the table spec, so the caller does not look it up twice.
    pub fn validate(&self) -> Result<&'static TableSpec> {
        if self.tenant.len() != 32 || !self.tenant.bytes().all(|byte| byte.is_ascii_hexdigit()) {
            return Err(Error::Invalid(
                "db ingest tenant must be 32 hex characters".to_string(),
            ));
        }
        if self.batch_limit == 0 {
            return Err(Error::Invalid(
                "db ingest batch limit must be at least 1".to_string(),
            ));
        }
        if self.batch_limit > MAX_BATCH_LIMIT {
            return Err(Error::Database(class::TOO_MANY_EVENTS));
        }
        // Default-deny: an unknown table, a secret table, and an allowlisted table
        // with no row mapping are three different refusals, all before any connect.
        allowlist::ensure_for_ingest(&self.table)
    }
}

/// What one pass did. Returned even for an empty page, so `db.ingest` can report
/// "connected, nothing new".
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PassReport {
    pub rows: usize,
    /// Rows that could not be decoded and were skipped (the cursor moved past them).
    pub skipped: usize,
    pub applied: u64,
    pub duplicates: u64,
    pub rejected: u64,
    pub high_water: u64,
    /// Cursor value after the pass.
    pub cursor: i64,
    pub latency_ms: u64,
}

impl PassReport {
    pub fn empty(cursor: i64) -> Self {
        Self {
            rows: 0,
            skipped: 0,
            applied: 0,
            duplicates: 0,
            rejected: 0,
            high_water: 0,
            cursor,
            latency_ms: 0,
        }
    }

    pub fn to_json(&self) -> String {
        format!(
            concat!(
                "{{\"rows\":{},\"skipped\":{},\"applied\":{},\"duplicates\":{},",
                "\"rejected\":{},\"high_water\":{},\"cursor\":{},\"latency_ms\":{}}}"
            ),
            self.rows,
            self.skipped,
            self.applied,
            self.duplicates,
            self.rejected,
            self.high_water,
            self.cursor,
            self.latency_ms
        )
    }
}

/// Health counters, published through `db.status` (§ 9 of `docs/transport.md`).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct IngestHealth {
    /// Wall clock of the last *successful* pass, including an empty one.
    pub last_ok_at: Option<u64>,
    /// Same instant, kept separately because `lag_ms` is defined as `now -
    /// last_read_ms`: when publication stops, this stops moving and the lag grows.
    pub last_read_ms: Option<u64>,
    pub lag_ms: Option<u64>,
    pub fails: u64,
    pub batches: u64,
    pub applied: u64,
    pub duplicates: u64,
    pub skipped: u64,
    pub rejected: u64,
    pub last_error: Option<&'static str>,
    pub last_key: Option<i64>,
    /// From the last successful connect.
    pub connected: bool,
    pub tables_ok: bool,
    pub latency_ms: u64,
}

impl IngestHealth {
    /// Adopt the connection-side facts from a source's own report.
    fn adopt(&mut self, status: &DbStatus) {
        self.connected = status.connected;
        self.tables_ok = status.tables_ok;
        self.latency_ms = status.latency_ms;
    }

    fn record_failure(&mut self, class: &'static str, now_ms: i64) {
        self.fails = self.fails.saturating_add(1);
        self.last_error = Some(class);
        self.connected = false;
        self.tables_ok = false;
        self.lag_ms = self
            .last_read_ms
            .map(|last| (now_ms.max(0) as u64).saturating_sub(last));
    }

    fn record_success(&mut self, now_ms: i64) {
        let now = now_ms.max(0) as u64;
        self.last_error = None;
        self.last_ok_at = Some(now);
        self.last_read_ms = Some(now);
        self.lag_ms = Some(0);
    }
}

/// The vault material the ingest needs, derived once per unlock.
///
/// The DEK itself is **not** kept: only the two labelled subkeys, so a memory dump of
/// the ingest state yields neither the tenant key nor a key that decrypts the
/// journal at rest (`LABEL_AT_REST`).
struct KeyMaterial {
    /// `subkey(DEK, LABEL_SYNC_DATA)` — what `ops.value` is sealed with.
    sync_key: SecretBytes,
    /// `subkey(DEK, LABEL_DB_CONN)` — what the connection string slot is sealed
    /// with.
    conn_key: SecretBytes,
    key_id: u16,
    algorithm: AeadAlgorithm,
}

impl KeyMaterial {
    fn derive(tenant_dek: &SecretBytes, key_id: u16) -> Result<Self> {
        Ok(Self {
            sync_key: kdf::subkey(tenant_dek, kdf::LABEL_SYNC_DATA)?,
            conn_key: credentials::sealing_key(tenant_dek)?,
            key_id,
            algorithm: AeadAlgorithm::XChaCha20Poly1305,
        })
    }
}

/// Builds a [`DbSource`] from the parsed facts. Production uses [`open_source`];
/// tests substitute a mock, which is the only seam that lets the orchestration be
/// tested without a live database.
pub type Connector = dyn Fn(&conninfo::ConnFacts, &SecretBytes, &'static TableSpec) -> Result<Box<dyn DbSource + Send>>
    + Send
    + Sync;

/// Connect with the real driver of the engine the facts name.
pub fn open_source(
    facts: &conninfo::ConnFacts,
    connection_string: &SecretBytes,
    spec: &'static TableSpec,
) -> Result<Box<dyn DbSource + Send>> {
    match facts.engine {
        crate::db::Engine::Postgres => Ok(Box::new(crate::db::postgres::PostgresSource::connect(
            facts,
            connection_string,
            spec,
        )?)),
        crate::db::Engine::MySql => Ok(Box::new(crate::db::mysql::MySqlSource::connect(
            facts,
            connection_string,
            spec,
        )?)),
    }
}

struct State {
    key: Option<KeyMaterial>,
    source: Option<Box<dyn DbSource + Send>>,
    health: IngestHealth,
}

/// The DB ingest: configuration, health, and at most one live connection.
pub struct DbIngest {
    config: DbIngestConfig,
    spec: &'static TableSpec,
    connector: Box<Connector>,
    logger: Logger,
    state: Mutex<State>,
}

impl DbIngest {
    pub fn new(config: DbIngestConfig, logger: Logger) -> Result<Self> {
        Self::with_connector(config, logger, Box::new(open_source))
    }

    /// [`DbIngest::new`] with an injected source factory (tests).
    pub fn with_connector(
        config: DbIngestConfig,
        logger: Logger,
        connector: Box<Connector>,
    ) -> Result<Self> {
        let spec = config.validate()?;
        Ok(Self {
            config,
            spec,
            connector,
            logger,
            state: Mutex::new(State {
                key: None,
                source: None,
                health: IngestHealth::default(),
            }),
        })
    }

    pub fn config(&self) -> &DbIngestConfig {
        &self.config
    }

    pub fn table(&self) -> &'static TableSpec {
        self.spec
    }

    pub fn interval_secs(&self) -> u64 {
        self.config.interval_secs
    }

    /// Derive the two subkeys the ingest needs. Called when the vault is unlocked;
    /// without it the ingest refuses with `Error::Auth` and the cursor does not move.
    pub fn unlock(&self, tenant_dek: &SecretBytes, key_id: u16) -> Result<()> {
        let key = KeyMaterial::derive(tenant_dek, key_id)?;
        let mut state = self
            .state
            .lock()
            .map_err(|_| Error::Storage("db ingest state is poisoned".to_string()))?;
        state.key = Some(key);
        // A connection opened under the previous key must not be reused: the health
        // view would say "connected" while the seal path uses a key the journal was
        // not written with.
        state.source = None;
        state.health.last_error = None;
        Ok(())
    }

    /// Drop the key material and the connection (vault locked). Idempotent.
    pub fn lock(&self) {
        if let Ok(mut state) = self.state.lock() {
            state.key = None;
            state.source = None;
        }
    }

    pub fn is_unlocked(&self) -> bool {
        self.state
            .lock()
            .map(|state| state.key.is_some())
            .unwrap_or(false)
    }

    pub fn health(&self) -> IngestHealth {
        self.state
            .lock()
            .map(|state| state.health.clone())
            .unwrap_or_default()
    }

    /// One ingest pass.
    ///
    /// Returns `Ok` when the read *and* the local write succeeded — including an
    /// empty page. Any error leaves the cursor untouched and is recorded in
    /// [`IngestHealth::last_error`] as a class.
    pub fn run_pass(&self, store: &mut Store, now_ms: i64) -> Result<PassReport> {
        let mut guard = self
            .state
            .lock()
            .map_err(|_| Error::Storage("db ingest state is poisoned".to_string()))?;
        let state = &mut *guard;

        // 1. Key material. Without it there is nothing to seal the rows with, and the
        //    connection string cannot be opened either.
        let Some(key) = state.key.as_ref() else {
            state.health.record_failure(class::VAULT_LOCKED, now_ms);
            return Err(Error::Auth(
                "vault is locked: the DB ingest needs the tenant DEK".to_string(),
            ));
        };

        // 2. Cursor.
        let cursor = crate::store::cursor::read(store, &self.config.account, self.config.source())?;
        let last_key = cursor.as_ref().map(|row| row.last_key).unwrap_or(0);

        // 3. Lazy connect.
        if state.source.is_none() {
            match self.connect(key) {
                Ok(source) => {
                    let status = source.status();
                    state.health.adopt(&status);
                    if !status.tables_ok {
                        // § 2.6 п. 7: "права недостаточны" is a distinct state from
                        // "БД недоступна", and the client shows both differently.
                        self.logger.warn(
                            "db-ingest",
                            &format!(
                                "table {} is not readable with the current grants",
                                self.spec.name
                            ),
                        );
                    }
                    state.source = Some(source);
                }
                Err(error) => {
                    state.health.record_failure(class_of(&error), now_ms);
                    return Err(error);
                }
            }
        }

        // 4. Page. The source is taken out of the state so a failure drops it: the
        //    next pass reconnects instead of reusing a connection whose socket may be
        //    dead.
        let Some(mut source) = state.source.take() else {
            state.health.record_failure(class::CONNECT, now_ms);
            return Err(Error::Database(class::CONNECT));
        };
        let rows = match source.next_batch(last_key, self.config.batch_limit) {
            Ok(rows) => {
                let status = source.status();
                state.health.adopt(&status);
                state.source = Some(source);
                rows
            }
            Err(error) => {
                state.health.record_failure(class_of(&error), now_ms);
                return Err(error);
            }
        };

        let latency_ms = state.health.latency_ms;
        if rows.is_empty() {
            state.health.record_success(now_ms);
            state.health.batches = state.health.batches.saturating_add(1);
            state.health.last_key = cursor.as_ref().map(|row| row.last_key);
            return Ok(PassReport {
                latency_ms,
                ..PassReport::empty(last_key)
            });
        }

        // 5. Seal. A sealing failure is *ours* (a key or a code bug), not a DB
        //    problem, so the batch is abandoned before anything is written.
        let mut skipped = 0usize;
        let mut events = Vec::with_capacity(rows.len());
        let seed = store
            .transaction(|tx| tx.device_last_hlc(&self.config.account, SERVER_DEVICE_ID))?
            .unwrap_or(Hlc::ZERO);
        let mut clock = DeviceClock::new(seed);
        for row in &rows {
            let DbRowBody::Event {
                kind,
                entity_id,
                field,
                value,
            } = &row.body
            else {
                skipped = skipped.saturating_add(1);
                continue;
            };
            let hlc = clock.stamp(row.observed_ms, now_ms);
            events.push(seal_row(
                row,
                *kind,
                entity_id,
                field.as_deref(),
                value,
                hlc,
                &self.config.account,
                key,
            )?);
        }

        // 6. One transaction: the journal rows and the cursor commit together.
        let max_key = rows.iter().map(|row| row.key).max().unwrap_or(last_key);
        let (outcome, advanced) = store.transaction(|tx| {
            let outcome = crate::store::ingest::ingest_batch_in_tx(
                tx,
                &self.config.account,
                &events,
                now_ms,
            )?;
            let cursor = crate::store::cursor::advance_in_tx(
                tx,
                &self.config.account,
                self.config.source(),
                max_key,
                now_ms,
            )?;
            Ok((outcome, cursor))
        })?;

        // 7. Health. `last_key` is what the store actually has, not what this pass
        //    asked for.
        state.health.record_success(now_ms);
        state.health.batches = state.health.batches.saturating_add(1);
        state.health.applied = state.health.applied.saturating_add(outcome.applied);
        state.health.duplicates = state.health.duplicates.saturating_add(outcome.duplicates);
        state.health.rejected = state.health.rejected.saturating_add(outcome.rejected);
        state.health.skipped = state
            .health
            .skipped
            .saturating_add(u64::try_from(skipped).unwrap_or(u64::MAX));
        state.health.last_key = Some(advanced.last_key);

        Ok(PassReport {
            rows: rows.len(),
            skipped,
            applied: outcome.applied,
            duplicates: outcome.duplicates,
            rejected: outcome.rejected,
            high_water: outcome.high_water,
            cursor: advanced.last_key,
            latency_ms,
        })
    }

    /// A `db.status` report (§ 5.8). Infallible: a status call must answer `200`
    /// even when the database is down or the vault is locked.
    ///
    /// When the process has never connected but the vault is unlocked, this makes one
    /// connection attempt — that is what makes `db.status` a real gate for the client
    /// (`docs/transport.md` § 5.9), and the attempt is bounded by
    /// [`DEFAULT_CONNECT_TIMEOUT_SECS`].
    pub fn status(&self, store: Option<&Store>, now_ms: u64) -> DbStatus {
        let Ok(mut guard) = self.state.lock() else {
            return DbStatus {
                last_error: Some(class::STORE),
                ..DbStatus::locked()
            };
        };

        // One connection attempt, if it could change the answer. Borrowing `key` and
        // `source`/`health` as separate fields keeps the secret in place instead of
        // copying it out of the state.
        let state = &mut *guard;
        if state.key.is_some() && state.source.is_none() {
            if let Some(key) = state.key.as_ref() {
                match self.connect(key) {
                    Ok(source) => {
                        let status = source.status();
                        state.health.adopt(&status);
                        state.source = Some(source);
                        // `last_error` is *not* cleared here: a successful reconnect
                        // after a failed pass still means the last pass published
                        // nothing. Only a real pass clears it (`record_success`).
                    }
                    Err(error) => {
                        let class = class_of(&error);
                        state
                            .health
                            .record_failure(class, i64::try_from(now_ms).unwrap_or(i64::MAX));
                    }
                }
            }
        }

        let state = &*guard;
        let unlocked = state.key.is_some();
        let from_source = state.source.as_ref().map(|source| source.status());
        let mut status = from_source.unwrap_or_else(|| DbStatus {
            engine: None,
            ..DbStatus::locked()
        });
        status.vault = if unlocked {
            crate::db::VaultState::Unlocked.as_str()
        } else {
            crate::db::VaultState::Locked.as_str()
        };
        status.last_ok_at = state.health.last_ok_at;
        status.last_read_ms = state.health.last_read_ms;
        status.fails = state.health.fails;
        status.batches = state.health.batches;
        status.applied = state.health.applied;
        status.duplicates = state.health.duplicates;
        status.skipped = state.health.skipped;
        if unlocked {
            // `lag_ms` is "how far publication lags": it grows while passes fail,
            // because `last_read_ms` stops moving.
            status.lag_ms = state
                .health
                .last_read_ms
                .map(|last| now_ms.saturating_sub(last));
            status.connected = state.health.connected;
            status.tables_ok = state.health.tables_ok;
            status.last_error = state.health.last_error;
        } else {
            status.connected = false;
            status.tables_ok = false;
            status.lag_ms = None;
            status.last_error = Some(class::VAULT_LOCKED);
        }
        // The cursor comes from the store, not from memory: it is the only value that
        // survives a restart, so it is the only value worth publishing.
        if let Some(store) = store {
            let row = crate::store::cursor::read(store, &self.config.account, self.config.source())
                .ok()
                .flatten();
            status = status.with_cursor(row.as_ref(), self.spec.key_column);
        }
        status
    }

    fn connect(&self, key: &KeyMaterial) -> Result<Box<dyn DbSource + Send>> {
        let stored =
            credentials::load_with_key(&self.config.data_dir, &self.config.tenant, &key.conn_key)?
                .ok_or(Error::Database(class::CREDENTIALS_MISSING))?;
        let text =
            std::str::from_utf8(stored.expose()).map_err(|_| Error::Database(class::CONFIG))?;
        let facts = conninfo::parse(text)?;
        if facts.ssl.is_weaker_than_verify_full() {
            // § 2.6 п. 6: weaker than `verify-full` is a warning, `disable` is a
            // refusal. The warning names the mode, never the string.
            self.logger.warn(
                "db-ingest",
                &format!(
                    "DB TLS mode is {} for {}; verify-full is the default",
                    facts.ssl.as_str(),
                    facts.redacted()
                ),
            );
        }
        (self.connector)(&facts, &stored, self.spec)
    }
}

/// Monotone HLC for the server's own device.
///
/// Strictly increasing, which is what `store::ingest` demands per `(account,
/// device)`. When the logical part is exhausted the physical part is bumped by one
/// millisecond rather than borrowing from the future silently — the same policy as
/// `sync::clock::MAX_LOGICAL`.
#[derive(Clone, Debug)]
pub struct DeviceClock {
    last: Hlc,
}

impl DeviceClock {
    pub fn new(last: Hlc) -> Self {
        Self { last }
    }

    pub fn last(&self) -> Hlc {
        self.last
    }

    /// Stamp one row: the physical part is the row's own observation time, never
    /// behind the previous stamp and never behind `now_ms`.
    pub fn stamp(&mut self, observed_ms: i64, now_ms: i64) -> Hlc {
        let observed = u64::try_from(observed_ms).unwrap_or(0).max(1);
        let now = u64::try_from(now_ms).unwrap_or(0);
        let physical = observed.max(now).max(self.last.physical_ms);
        let next = if physical > self.last.physical_ms {
            Hlc::new(physical, 0)
        } else if self.last.logical < MAX_LOGICAL {
            Hlc::new(physical, self.last.logical.saturating_add(1))
        } else {
            Hlc::new(physical.saturating_add(1), 0)
        };
        self.last = next;
        next
    }
}

/// `content_hash` of a row, computed from the **row**, not from the journal HLC.
///
/// # Why not `Event::compute_content_hash`
///
/// That one mixes in the journal clock, which must keep advancing per device — so the
/// same DB row read twice would hash differently and `UNIQUE(account_id,
/// content_hash)` would not catch the duplicate. Here the clock is the row's own
/// `observed_ms`, so the hash is stable for the row's lifetime:
/// "the same fact observed at the same time on the same field".
///
/// # What is hashed
///
/// The cleartext value, not the ciphertext (the AEAD nonce is fresh every pass; a
/// deterministic nonce would be worse than this trade-off). The consequence is
/// documented in `docs/db-ingest.md` § 6: someone who steals `ops` can test "was this
/// exact value in the DB" against guesses. The same is already true of `content_hash`
/// on the client path for anyone who knows the plaintext.
pub fn row_content_hash(row: &DbRow) -> [u8; 32] {
    let (kind, entity_id, field, value) = match &row.body {
        DbRowBody::Event {
            kind,
            entity_id,
            field,
            value,
        } => (
            Some(*kind),
            entity_id.as_str(),
            field.as_deref(),
            value.as_slice(),
        ),
        DbRowBody::Malformed => (None, "", None, &[][..]),
    };
    let observed = u64::try_from(row.observed_ms).unwrap_or(0);
    crate::store::event::content_hash(kind, entity_id, field, value, Hlc::new(observed, 0))
}

/// AAD that binds a sealed value to the address it was read from.
///
/// Mirrors `sync::model::FieldRecord::aad`: entity + field + delete flag, plus a
/// domain tag so a ciphertext from the client path cannot be passed off as one from
/// the DB path. A value moved to another entity or field fails to open.
pub fn value_aad(kind: EventKind, entity_id: &str, field: Option<&str>) -> Vec<u8> {
    let mut aad = Vec::with_capacity(16 + entity_id.len() + 16);
    aad.extend_from_slice(b"tf/db-ingest/v1\0");
    aad.push(kind.code());
    aad.extend_from_slice(&(entity_id.len() as u32).to_be_bytes());
    aad.extend_from_slice(entity_id.as_bytes());
    match field {
        Some(field) => {
            aad.push(1);
            aad.extend_from_slice(&(field.len() as u32).to_be_bytes());
            aad.extend_from_slice(field.as_bytes());
        }
        None => aad.push(0),
    }
    aad
}

/// Seal one row into the event that will be journalled.
///
/// # The presence exception
///
/// `store::ingest` validates a presence event's `value` as *exactly* one big-endian
/// `i64` (it projects it into `presence.detail_ms`), so a sealed envelope — which is
/// longer than 8 bytes — would be rejected as `malformed_presence_payload`. Presence is
/// therefore journalled as its 8-byte payload, exactly as the client's `sync.push`
/// path does: what is stored is "online till / last seen", a timestamp, not message
/// content. Everything that carries text or media (`MessageNew`/`MessageEdit`/
/// `MessageDelete`, `ProfileUpdate`, `ArchiveObservation`) is sealed with
/// `subkey(DEK, LABEL_SYNC_DATA)`.
#[allow(clippy::too_many_arguments)]
fn seal_row(
    row: &DbRow,
    kind: EventKind,
    entity_id: &str,
    field: Option<&str>,
    value: &[u8],
    hlc: Hlc,
    account: &AccountId,
    key: &KeyMaterial,
) -> Result<Event> {
    let stored = match kind {
        EventKind::PresenceOnline | EventKind::PresenceOffline => value.to_vec(),
        _ => aead::seal(
            &key.sync_key,
            key.key_id,
            key.algorithm,
            &value_aad(kind, entity_id, field),
            value,
        )
        .map_err(|_| Error::Database(class::SEAL))?,
    };
    let mut event = Event::new(
        *account,
        SERVER_DEVICE_ID,
        kind,
        entity_id,
        field,
        stored,
        hlc,
    );
    event.content_hash = row_content_hash(row);
    Ok(event)
}

/// Run one pass and log it. The scheduler and `db.ingest` both go through this, so the
/// logging policy lives in one place: **class only**, never a value, never a row.
pub fn run_and_log(ingest: &DbIngest, store: &mut Store, now_ms: i64) -> Result<PassReport> {
    match ingest.run_pass(store, now_ms) {
        Ok(report) => {
            if report.rows == 0 {
                ingest
                    .logger
                    .log(crate::logging::Level::Debug, "db-ingest", &report.to_json());
            } else {
                ingest.logger.info(
                    "db-ingest",
                    &format!("published {} table={}", report.to_json(), ingest.spec.name),
                );
            }
            Ok(report)
        }
        Err(error) => {
            // The class, not the message: `error.to_string()` would be "database
            // error: connect" for a DB failure (safe), but the same call on a storage
            // error carries SQLite text. The class is the contract.
            ingest.logger.error(
                "db-ingest",
                &format!(
                    "pass failed class={} table={}",
                    class_of(&error),
                    ingest.spec.name
                ),
            );
            Err(error)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::crypto::aead;
    use crate::db::mock::MockSource;
    use crate::db::{DbStatus, VaultState};
    use crate::logging::{Destination, Level, Logger};
    use crate::store::query;
    use crate::store::AccountId;
    use crate::test_support::TestDir;

    const TENANT: &str = "00112233445566778899aabbccddeeff";
    const ACCOUNT: [u8; 16] = [0x37; 16];
    const NOW: i64 = 1_800_000_000_000;

    fn account() -> AccountId {
        AccountId::new(ACCOUNT).unwrap()
    }

    /// A DEK that is not derived from a passphrase: the ingest only needs a 32-byte
    /// key, and running Argon2id here would test Argon2id.
    fn dek() -> SecretBytes {
        SecretBytes::new(vec![0x6d; 32])
    }

    fn logger() -> Logger {
        // `Error` keeps the expected-failure tests quiet; the lines that matter are
        // asserted through health/status, not through stderr.
        Logger::new(Level::Error, Destination::Stderr)
    }

    fn message_row(key: i64, entity: &str, field: &str, value: &[u8]) -> DbRow {
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

    fn presence_row(key: i64, peer: i64) -> DbRow {
        DbRow {
            key,
            observed_ms: 1_700_000_000_000 + key,
            body: DbRowBody::Event {
                kind: EventKind::PresenceOnline,
                entity_id: format!("presence:{peer}"),
                field: None,
                value: (1_700_000_000_000i64 + key).to_be_bytes().to_vec(),
            },
        }
    }

    fn connector_for(rows: Vec<DbRow>) -> Box<Connector> {
        Box::new(move |_facts, _conn, _spec| Ok(Box::new(MockSource::new(rows.clone()))))
    }

    fn connector_failing(class: &'static str) -> Box<Connector> {
        Box::new(move |_facts, _conn, _spec| Ok(Box::new(MockSource::failing(class))))
    }

    /// Everything a pass needs: a store on disk (so "restart" is real), a vault slot
    /// with a connection string, and an unlocked ingest.
    struct Fixture {
        directory: TestDir,
        store: Store,
        ingest: DbIngest,
    }

    impl Fixture {
        fn with_connector(name: &str, connector: Box<Connector>) -> Self {
            let directory = TestDir::new(name);
            let data_dir = directory.path().to_string_lossy().to_string();
            let store = Store::open(directory.join("data.db")).unwrap();
            let config = DbIngestConfig {
                interval_secs: 0,
                ..DbIngestConfig::new(data_dir.clone(), TENANT, account())
            };
            // The slot is part of the test on purpose: the ingest must go through the
            // vault, not around it.
            credentials::store(
                &data_dir,
                TENANT,
                &dek(),
                1,
                &SecretBytes::new(b"postgresql://teleforge_agent:pw@db:5432/tf".to_vec()),
            )
            .unwrap();
            let ingest = DbIngest::with_connector(config, logger(), connector).unwrap();
            ingest.unlock(&dek(), 1).unwrap();
            Self {
                directory,
                store,
                ingest,
            }
        }

        fn with_rows(name: &str, rows: Vec<DbRow>) -> Self {
            Self::with_connector(name, connector_for(rows))
        }

        fn data_dir(&self) -> String {
            self.directory.path().to_string_lossy().to_string()
        }

        fn pass(&mut self, now_ms: i64) -> Result<PassReport> {
            self.ingest.run_pass(&mut self.store, now_ms)
        }

        fn status(&self, now_ms: u64) -> DbStatus {
            self.ingest.status(Some(&self.store), now_ms)
        }

        fn ops(&self) -> Vec<crate::store::query::StoredOp> {
            query::ops_since(&self.store, &account(), 0, 100).unwrap()
        }
    }

    #[test]
    fn a_pass_publishes_sealed_rows_into_the_journal() {
        // The whole cycle of `docs/db-ingest.md` § 1 in one test: cursor → page →
        // seal → ops → the exact rows `sync.pull` would hand a client.
        let mut fixture = Fixture::with_rows(
            "dbingest",
            vec![
                message_row(1, "message:-1001234567890:42", "text", b"hello"),
                presence_row(2, 900),
            ],
        );
        let report = fixture.pass(NOW).expect("the first pass must succeed");
        assert_eq!(report.rows, 2);
        assert_eq!(report.applied, 2);
        assert_eq!(report.duplicates, 0);
        assert_eq!(report.cursor, 2);
        assert_eq!(report.high_water, 2);

        let ops = fixture.ops();
        assert_eq!(ops.len(), 2);
        // The journal attributes them to the server's own device, not to a client.
        assert!(ops.iter().all(|op| op.device_id == SERVER_DEVICE_ID));
        // The message value in the journal is an AEAD envelope, not the plaintext.
        let message = ops
            .iter()
            .find(|op| op.entity_id == "message:-1001234567890:42")
            .expect("the message row must be journalled");
        assert!(message.value.starts_with(&aead::MAGIC));
        assert!(!message.value.windows(5).any(|window| window == b"hello"));
        // Presence is the documented exception: the payload *is* the timestamp, which
        // `store::ingest` validates and projects as one big-endian `i64`.
        let presence = ops
            .iter()
            .find(|op| op.entity_id == "presence:900")
            .expect("the presence row must be journalled");
        assert_eq!(presence.value.len(), 8);
        assert_eq!(
            i64::from_be_bytes(
                <[u8; 8]>::try_from(presence.value.as_slice()).expect("checked length above")
            ),
            1_700_000_000_002
        );

        // ... and the message opens with the key the contract names:
        // subkey(DEK, LABEL_SYNC_DATA), with the row's address as AAD.
        let sync_key = kdf::subkey(&dek(), kdf::LABEL_SYNC_DATA).unwrap();
        let plain = aead::open(
            &sync_key,
            &value_aad(message.kind, &message.entity_id, message.field.as_deref()),
            &message.value,
        )
        .expect("the sealed value must open with the sync subkey");
        assert_eq!(plain.expose(), b"hello");
        assert_eq!(message.kind, EventKind::MessageNew);
        assert_eq!(message.field.as_deref(), Some("text"));
    }

    #[test]
    fn a_sealed_value_cannot_be_moved_to_another_entity() {
        let sync_key = kdf::subkey(&dek(), kdf::LABEL_SYNC_DATA).unwrap();
        let sealed = aead::seal(
            &sync_key,
            1,
            AeadAlgorithm::XChaCha20Poly1305,
            &value_aad(EventKind::MessageNew, "message:1:1", Some("text")),
            b"secret",
        )
        .unwrap();
        // The AAD binds the ciphertext to its address, like `FieldRecord::aad`.
        assert!(aead::open(
            &sync_key,
            &value_aad(EventKind::MessageNew, "message:1:2", Some("text")),
            &sealed
        )
        .is_err());
        assert!(aead::open(
            &sync_key,
            &value_aad(EventKind::MessageNew, "message:1:1", Some("views")),
            &sealed
        )
        .is_err());
        assert!(aead::open(
            &sync_key,
            &value_aad(EventKind::MessageNew, "message:1:1", Some("text")),
            &sealed
        )
        .is_ok());
    }

    #[test]
    fn a_second_pass_reads_nothing_new_and_keeps_the_cursor() {
        let mut fixture = Fixture::with_rows(
            "dbingest",
            vec![message_row(1, "message:1:1", "text", b"a")],
        );
        fixture.pass(NOW).unwrap();
        let second = fixture.pass(NOW + 1_000).unwrap();
        assert_eq!(second.rows, 0);
        assert_eq!(second.cursor, 1, "an empty page must not move the cursor");
        assert_eq!(fixture.ops().len(), 1);
    }

    #[test]
    fn a_restart_continues_from_the_cursor_and_does_not_duplicate() {
        // The migration v3 guarantee, end to end: the cursor survives the process.
        let rows = vec![
            message_row(1, "message:1:1", "text", b"a"),
            message_row(2, "message:1:2", "text", b"b"),
            message_row(3, "message:1:3", "text", b"c"),
        ];
        let mut fixture = Fixture::with_rows("dbingest", rows.clone());
        assert_eq!(fixture.pass(NOW).unwrap().applied, 3);
        assert_eq!(fixture.ops().len(), 3);
        let data_dir = fixture.data_dir();
        let path = fixture.store.path().to_path_buf();
        // Drop the process state: store and ingest both go away.
        let report = fixture.pass(NOW + 1).unwrap();
        assert_eq!(report.rows, 0);
        drop(fixture.store);
        drop(fixture.ingest);

        // A new process: same data dir, same slot, fresh ingest object.
        let store = Store::open(&path).unwrap();
        let config = DbIngestConfig::new(data_dir.clone(), TENANT, account());
        credentials::store(
            &data_dir,
            TENANT,
            &dek(),
            1,
            &SecretBytes::new(b"postgresql://teleforge_agent:pw@db:5432/tf".to_vec()),
        )
        .unwrap();
        let ingest =
            DbIngest::with_connector(config, logger(), connector_for(rows.clone())).unwrap();
        ingest.unlock(&dek(), 1).unwrap();
        // The source has all three rows again; the store says they are published.
        let cursor = crate::store::cursor::read(&store, &account(), DEFAULT_TABLE)
            .unwrap()
            .expect("the cursor must have survived the restart");
        assert_eq!(cursor.last_key, 3);
        // Reading after the cursor yields nothing, so the pass publishes nothing.
        let mut store = store;
        let report = ingest.run_pass(&mut store, NOW + 2).unwrap();
        assert_eq!(report.rows, 0);
        assert_eq!(report.cursor, 3);
        assert_eq!(query::op_count(&store, &account()).unwrap(), 3);
        // ... and a fresh read of the whole table from the start would still dedupe,
        // because the content hash is computed from the row, not from the clock.
        let mut store2 = store;
        ingest
            .run_pass(&mut store2, NOW + 3)
            .expect("an empty pass is still a success");
        assert_eq!(query::op_count(&store2, &account()).unwrap(), 3);
    }

    #[test]
    fn a_row_read_twice_hashes_the_same_and_is_a_duplicate() {
        // Two sources over the same row (a re-read after a lost cursor, or a
        // table that was replayed from the beginning) must not double the journal.
        let rows = vec![message_row(7, "message:1:7", "text", b"same")];
        let mut fixture = Fixture::with_rows("dbingest", rows.clone());
        fixture.pass(NOW).unwrap();
        // Rewind the cursor by hand: the worst case the dedupe key exists for.
        crate::store::cursor::clear(&mut fixture.store, &account(), DEFAULT_TABLE).unwrap();
        let replayed = fixture.pass(NOW + 1_000).unwrap();
        assert_eq!(replayed.rows, 1);
        assert_eq!(replayed.applied, 0);
        assert_eq!(replayed.duplicates, 1);
        assert_eq!(query::op_count(&fixture.store, &account()).unwrap(), 1);
    }

    #[test]
    fn the_row_hash_ignores_the_journal_clock() {
        let row = message_row(1, "message:1:1", "text", b"x");
        let first = row_content_hash(&row);
        // Same row, different wall clock: the hash must not move.
        let mut other = row.clone();
        other.observed_ms = 1;
        assert_ne!(
            first,
            row_content_hash(&other),
            "observed_ms is part of the fact"
        );
        assert_eq!(first, row_content_hash(&row.clone()));
        // A changed value is a different fact.
        let mut changed = row.clone();
        changed.body = message_row(1, "message:1:1", "text", b"y").body;
        assert_ne!(first, row_content_hash(&changed));
    }

    #[test]
    fn the_server_clock_is_strictly_monotonic_across_rows_and_passes() {
        let mut clock = DeviceClock::new(Hlc::ZERO);
        // Rows read in descending observation order still get increasing stamps.
        let mut previous = clock.stamp(1_000, 2_000);
        for observed in [900, 900, 1_000, 500] {
            let next = clock.stamp(observed, 2_000);
            assert!(next > previous, "{next:?} must be after {previous:?}");
            previous = next;
        }
        // ... including after an explicit seed, which is what a restart looks like.
        let mut clock = DeviceClock::new(Hlc::new(5_000, 3));
        let next = clock.stamp(1_000, 2_000);
        assert!(next > Hlc::new(5_000, 3));
        // A logical counter that is exhausted borrows a millisecond instead of
        // silently repeating a stamp the journal would reject.
        let mut clock = DeviceClock::new(Hlc::new(1_000, MAX_LOGICAL));
        assert_eq!(clock.stamp(1_000, 1_000), Hlc::new(1_001, 0));
    }

    #[test]
    fn a_database_error_leaves_the_cursor_alone_and_keeps_working() {
        let mut fixture = Fixture::with_connector("dbingest", connector_failing(class::QUERY));
        let error = fixture
            .pass(NOW)
            .expect_err("the pass must report the failure");
        assert_eq!(class_of(&error), class::QUERY);
        assert!(
            crate::store::cursor::read(&fixture.store, &account(), DEFAULT_TABLE)
                .unwrap()
                .is_none(),
            "a failed pass must not create or move the cursor"
        );
        let health = fixture.ingest.health();
        assert_eq!(health.fails, 1);
        assert_eq!(health.last_error, Some(class::QUERY));
        assert!(health.last_ok_at.is_none());

        // The process is still usable: a status call answers, and it says what
        // happened without inventing a connection.
        let status = fixture.status(NOW as u64);
        assert!(!status.connected);
        assert_eq!(status.last_error, Some(class::QUERY));
        assert_eq!(status.fails, 1);
        assert_eq!(status.vault, VaultState::Unlocked.as_str());

        // A source that starts working is picked up by the next pass.
        let mut healthy = Fixture::with_rows(
            "dbingest",
            vec![message_row(1, "message:1:1", "text", b"a")],
        );
        assert_eq!(healthy.pass(NOW).unwrap().applied, 1);
    }

    #[test]
    fn a_transient_failure_does_not_skip_the_rows_it_covered() {
        // The shape of "the socket dropped mid-page": the first pass fails, the
        // cursor stays, the retry reads the same rows. The connector remembers the
        // attempt (the ingest drops the connection after a failure and reconnects, so
        // a per-source `fail_once` would fail every time).
        let attempts = std::sync::Arc::new(std::sync::atomic::AtomicUsize::new(0));
        let counter = std::sync::Arc::clone(&attempts);
        let mut fixture = Fixture::with_connector(
            "dbingest",
            Box::new(move |_facts, _conn, _spec| {
                let source = MockSource::new(vec![
                    message_row(1, "message:1:1", "text", b"a"),
                    message_row(2, "message:1:2", "text", b"b"),
                ]);
                let attempt = counter.fetch_add(1, std::sync::atomic::Ordering::SeqCst);
                Ok(Box::new(if attempt == 0 {
                    source.fail_once(class::CONNECT)
                } else {
                    source
                }))
            }),
        );
        assert!(fixture.pass(NOW).is_err());
        let report = fixture.pass(NOW + 1).unwrap();
        assert_eq!(
            report.rows, 2,
            "the retry must read the rows it never wrote"
        );
        assert_eq!(report.applied, 2);
        assert_eq!(report.cursor, 2);
        assert_eq!(fixture.ops().len(), 2);
    }

    #[test]
    fn a_locked_vault_refuses_the_pass_without_touching_the_cursor() {
        let mut fixture = Fixture::with_rows(
            "dbingest",
            vec![message_row(1, "message:1:1", "text", b"a")],
        );
        fixture.ingest.lock();
        let error = fixture.pass(NOW).expect_err("a locked vault must refuse");
        assert!(
            matches!(error, Error::Auth(_)),
            "the contract says Auth, got {error}"
        );
        assert!(
            crate::store::cursor::read(&fixture.store, &account(), DEFAULT_TABLE)
                .unwrap()
                .is_none()
        );
        let status = fixture.status(NOW as u64);
        assert_eq!(status.vault, VaultState::Locked.as_str());
        assert_eq!(status.last_error, Some(class::VAULT_LOCKED));
        assert!(!status.connected);

        // Unlocking makes the very same pass work.
        fixture.ingest.unlock(&dek(), 1).unwrap();
        assert_eq!(fixture.pass(NOW + 1).unwrap().applied, 1);
    }

    #[test]
    fn a_missing_connection_string_is_reported_as_a_configuration_state() {
        let directory = TestDir::new("dbingest");
        let data_dir = directory.path().to_string_lossy().to_string();
        let mut store = Store::open(directory.join("data.db")).unwrap();
        let config = DbIngestConfig::new(data_dir, TENANT, account());
        let ingest =
            DbIngest::with_connector(config, logger(), connector_failing(class::CONNECT)).unwrap();
        ingest.unlock(&dek(), 1).unwrap();
        // No `credentials::store`, so the slot is absent — and the connector must
        // never be reached.
        let error = ingest.run_pass(&mut store, NOW).unwrap_err();
        assert_eq!(class_of(&error), class::CREDENTIALS_MISSING);
        assert!(!ingest.health().connected);
    }

    #[test]
    fn a_malformed_row_is_skipped_and_the_cursor_moves_past_it() {
        let mut fixture = Fixture::with_rows(
            "dbingest",
            vec![
                message_row(1, "message:1:1", "text", b"a"),
                DbRow {
                    key: 2,
                    observed_ms: 1_700_000_000_002,
                    body: DbRowBody::Malformed,
                },
                message_row(3, "message:1:3", "text", b"c"),
            ],
        );
        let report = fixture.pass(NOW).unwrap();
        assert_eq!(report.rows, 3);
        assert_eq!(report.skipped, 1);
        assert_eq!(report.applied, 2);
        assert_eq!(report.cursor, 3, "one bad row must not wedge the ingest");
        assert_eq!(fixture.ingest.health().skipped, 1);
    }

    #[test]
    fn the_page_size_is_bounded_and_the_cursor_follows_the_page() {
        let rows: Vec<DbRow> = (1..=5)
            .map(|key| message_row(key, &format!("message:1:{key}"), "text", b"x"))
            .collect();
        let directory = TestDir::new("dbingest");
        let data_dir = directory.path().to_string_lossy().to_string();
        let mut store = Store::open(directory.join("data.db")).unwrap();
        let config = DbIngestConfig {
            interval_secs: 0,
            batch_limit: 2,
            ..DbIngestConfig::new(data_dir.clone(), TENANT, account())
        };
        credentials::store(
            &data_dir,
            TENANT,
            &dek(),
            1,
            &SecretBytes::new(b"postgresql://u:p@db/tf".to_vec()),
        )
        .unwrap();
        let ingest = DbIngest::with_connector(config, logger(), connector_for(rows)).unwrap();
        ingest.unlock(&dek(), 1).unwrap();
        for (expected_cursor, expected_applied) in [(2, 2), (4, 2), (5, 1)] {
            let report = ingest.run_pass(&mut store, NOW).unwrap();
            assert_eq!(report.rows, if expected_applied == 1 { 1 } else { 2 });
            assert_eq!(report.applied, expected_applied);
            assert_eq!(report.cursor, expected_cursor);
        }
        let report = ingest.run_pass(&mut store, NOW).unwrap();
        assert_eq!(report.rows, 0);
        assert_eq!(query::op_count(&store, &account()).unwrap(), 5);
    }

    #[test]
    fn the_health_counters_describe_the_passes() {
        let mut fixture = Fixture::with_rows(
            "dbingest",
            vec![message_row(1, "message:1:1", "text", b"a")],
        );
        fixture.pass(NOW).unwrap();
        fixture.pass(NOW + 1_000).unwrap();
        let health = fixture.ingest.health();
        assert_eq!(health.batches, 2);
        assert_eq!(health.applied, 1);
        assert_eq!(health.fails, 0);
        assert_eq!(health.last_ok_at, Some((NOW + 1_000) as u64));
        assert_eq!(health.last_key, Some(1));
        assert_eq!(health.lag_ms, Some(0));
        assert!(health.connected);
        assert!(health.tables_ok);

        // `lag_ms` is "how far publication lags": once the last pass is in the past,
        // it grows.
        let status = fixture.status((NOW + 6_000) as u64);
        assert_eq!(status.lag_ms, Some(5_000));
        assert_eq!(status.batches, 2);
        assert_eq!(status.cursor.as_ref().map(|cursor| cursor.value), Some(1));
    }

    #[test]
    fn the_status_reports_a_missing_table_grant_separately() {
        let directory = TestDir::new("dbingest");
        let data_dir = directory.path().to_string_lossy().to_string();
        let mut store = Store::open(directory.join("data.db")).unwrap();
        let config = DbIngestConfig::new(data_dir.clone(), TENANT, account());
        credentials::store(
            &data_dir,
            TENANT,
            &dek(),
            1,
            &SecretBytes::new(b"postgresql://u:p@db/tf".to_vec()),
        )
        .unwrap();
        let ingest = DbIngest::with_connector(
            config,
            logger(),
            Box::new(|_facts, _conn, _spec| {
                Ok(Box::new(MockSource::new(Vec::new()).with_tables_ok(false)))
            }),
        )
        .unwrap();
        ingest.unlock(&dek(), 1).unwrap();
        ingest.run_pass(&mut store, NOW).unwrap();
        let status = ingest.status(Some(&store), NOW as u64);
        // "БД доступна, но прав недостаточно" is a distinct state (§ 2.6 п. 7).
        assert!(status.connected);
        assert!(!status.tables_ok);
        assert_eq!(status.last_error, None);
    }

    #[test]
    fn a_page_of_only_malformed_rows_still_registers_the_account_and_moves_the_cursor() {
        // The degenerate path: no events to ingest, but the cursor must still move and
        // the account row must exist for the `db_cursor` foreign key.
        let mut fixture = Fixture::with_rows(
            "dbingest",
            vec![
                DbRow {
                    key: 1,
                    observed_ms: 1,
                    body: DbRowBody::Malformed,
                },
                DbRow {
                    key: 2,
                    observed_ms: 2,
                    body: DbRowBody::Malformed,
                },
            ],
        );
        let report = fixture.pass(NOW).unwrap();
        assert_eq!(report.rows, 2);
        assert_eq!(report.skipped, 2);
        assert_eq!(report.applied, 0);
        assert_eq!(report.cursor, 2);
        assert_eq!(query::op_count(&fixture.store, &account()).unwrap(), 0);
        assert!(query::accounts(&fixture.store)
            .unwrap()
            .iter()
            .any(|info| info.account_id == account()));
    }

    #[test]
    fn a_config_that_names_a_secret_or_unknown_table_is_refused() {
        // Default-deny: a table nobody allowlisted, and the secret table by name.
        for table in ["personality_core", "users"] {
            let config = DbIngestConfig {
                table: table.to_string(),
                ..DbIngestConfig::new("/tmp", TENANT, account())
            };
            let error = config.validate().expect_err("the table must be refused");
            assert_eq!(class_of(&error), class::TABLE_NOT_ALLOWED, "{table}");
        }
        // Allowlisted, but with no row mapping in this build (`messages` needs the
        // per-column mapping of § 12.9).
        for table in ["messages", "peer_archive_name"] {
            let config = DbIngestConfig {
                table: table.to_string(),
                ..DbIngestConfig::new("/tmp", TENANT, account())
            };
            assert_eq!(
                class_of(&config.validate().unwrap_err()),
                class::UNSUPPORTED_TABLE,
                "{table}"
            );
        }
        let too_big = DbIngestConfig {
            batch_limit: MAX_BATCH_LIMIT + 1,
            ..DbIngestConfig::new("/tmp", TENANT, account())
        };
        assert_eq!(
            class_of(&too_big.validate().unwrap_err()),
            class::TOO_MANY_EVENTS
        );
        let bad_tenant = DbIngestConfig {
            tenant: "nope".to_string(),
            ..DbIngestConfig::new("/tmp", TENANT, account())
        };
        assert!(bad_tenant.validate().is_err());
        // The default is valid and names the outbox.
        let ok = DbIngestConfig::new("/tmp", TENANT, account());
        assert_eq!(ok.source(), "sync_outbox");
        assert!(ok.validate().is_ok());
    }

    #[test]
    fn the_pass_report_json_is_stable() {
        let report = PassReport {
            rows: 2,
            skipped: 1,
            applied: 1,
            duplicates: 1,
            rejected: 0,
            high_water: 9,
            cursor: 41,
            latency_ms: 3,
        };
        assert_eq!(
            report.to_json(),
            "{\"rows\":2,\"skipped\":1,\"applied\":1,\"duplicates\":1,\"rejected\":0,\
             \"high_water\":9,\"cursor\":41,\"latency_ms\":3}"
        );
    }
}
