//! Listener, connection lifecycle and the retention scheduler.
//!
//! # Bind policy
//!
//! The agent is installed and reached *by the desktop client*, over the SSH
//! connection the client already has, and the client forwards a loopback port or
//! proxies the unix socket. Therefore:
//!
//! * default is `127.0.0.1:8137` (TLS required);
//! * a wildcard bind is refused by [`crate::config::AgentConfig::validate`];
//! * `listen = "network"` additionally requires a client CA (mTLS);
//! * `listen = "socket"` is not wired yet and is refused rather than silently
//!   falling back to TCP — a fallback would be a different, unintended exposure.
//!
//! There is no "open a port so the client can reach me directly" mode. A VPS with an
//! agent port exposed to the internet is a session-stealing service; the installation
//! flow exists precisely to avoid it.
//!
//! # Threads, not a runtime
//!
//! One thread per connection, bounded by `max_connections`, and one retention thread
//! per process. No async runtime is pulled in: the store is single-owner and
//! synchronous (`rusqlite`), so an async listener would have to `spawn_blocking`
//! every request anyway, and a thread that holds the lock for the duration of one
//! batch is simpler to reason about than a task that might.
//!
//! A connection serves **exactly one request** (`Connection: close`). That is a
//! deliberate simplification: it removes keep-alive state, request pipelining and
//! per-connection idle timeouts from the attack surface, and costs one TCP handshake
//! per batch. The client is expected to send 2000 events per push, not one.
//!
//! # Limits, all enforced before the work
//!
//! | Limit | Where | Failure |
//! |---|---|---|
//! | body size | `api::http::read_request` before allocation | 413 |
//! | header line/count | same | 413/400 |
//! | concurrent connections | [`ConnectionLimiter`] at accept | 503 |
//! | per-device request rate | `api::rpc` after auth | 429 |
//! | batch size | `api::rpc::parse_events` | 400 |
//! | read/write timeouts | `TcpStream::set_*_timeout` | connection dropped |
//! | single event value size | `store::ingest` per class | per-event `Rejected` |

use std::io::{BufReader, Read, Write};
use std::net::{SocketAddr, TcpListener, TcpStream};
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};

use rustls::ServerConfig;

use crate::api::http::{self, HttpLimits};
use crate::api::rpc;
use crate::api::sync::SyncService;
use crate::api::tls::TlsIdentity;
use crate::auth::AuthStore;
use crate::config::{AgentConfig, ListenMode};
use crate::error::{Error, Result};
use crate::logging::{Level, Logger};
use crate::sync::clock::{Clock, SystemClock};

/// How often the accept loop checks the shutdown flag. Short enough that `systemd
/// stop` is not perceived as a hang, long enough that an idle agent is not a busy
/// loop.
const ACCEPT_POLL: std::time::Duration = std::time::Duration::from_millis(20);

/// Everything a request handler needs, shared by every connection.
pub struct ServerState {
    pub config: AgentConfig,
    pub limits: HttpLimits,
    pub started_at_ms: u64,
    /// Device records. A `Mutex` because `authenticate` records `last_seen_at`;
    /// the critical section is a linear scan of a handful of records.
    pub authorizer: Mutex<AuthStore>,
    /// Per-device token buckets.
    pub rate: Mutex<std::collections::BTreeMap<String, crate::auth::RateLimiter>>,
    pub idempotency: Mutex<IdempotencyStore>,
    pub sync: Arc<SyncService>,
    /// The server-side DB ingest, when it is configured. `None` means the agent was
    /// started without a tenant/account pair for the DB path — `db.status` then says
    /// so (`last_error: "config"`) instead of pretending the DB is down.
    pub db: Option<Arc<crate::db::DbIngest>>,
    pub logger: Logger,
    /// Flipped to false by `shutdown` or by a termination signal; the accept loop
    /// observes it between connections, never mid-request.
    pub running: Arc<AtomicBool>,
}

impl ServerState {
    pub fn new(config: AgentConfig) -> Result<Self> {
        let limits = HttpLimits {
            max_body_bytes: config.max_body_bytes,
            ..HttpLimits::default()
        };
        let level = Level::parse(&config.log_level).unwrap_or(Level::Info);
        let authorizer = AuthStore::load(&config.data_dir)?;
        let sync = Arc::new(SyncService::new(config.data_dir.clone(), config.retention));
        let logger = Logger::new(level, crate::logging::Destination::Stderr);
        // The DB ingest is built only when its tenant/account pair is configured:
        // without it there is nothing to publish into and `db.status` has no truthful
        // answer. `interval_secs == 0` still builds it — `db.ingest` and `db.status`
        // keep working, which is what an operator uses to test the grants first.
        let db = match (&config.db_ingest.tenant, &config.db_ingest.account) {
            (Some(tenant), Some(account)) => {
                let account = crate::store::AccountId::from_hex(account)?;
                let settings = crate::db::DbIngestConfig {
                    data_dir: config.data_dir.clone(),
                    tenant: tenant.clone(),
                    account,
                    interval_secs: config.db_ingest.interval_secs,
                    table: config.db_ingest.table.clone(),
                    batch_limit: config.db_ingest.batch_limit,
                };
                Some(Arc::new(crate::db::DbIngest::new(
                    settings,
                    Logger::new(level, crate::logging::Destination::Stderr),
                )?))
            }
            _ => None,
        };
        Ok(Self {
            config,
            limits,
            started_at_ms: SystemClock.now_ms(),
            authorizer: Mutex::new(authorizer),
            rate: Mutex::new(std::collections::BTreeMap::new()),
            idempotency: Mutex::new(IdempotencyStore::new(4096, 10 * 60 * 1_000)),
            sync,
            db,
            logger,
            running: Arc::new(AtomicBool::new(true)),
        })
    }

    pub fn shutdown(&self) {
        self.running.store(false, Ordering::SeqCst);
    }

    pub fn is_running(&self) -> bool {
        self.running.load(Ordering::SeqCst)
    }

    /// The listener's certificate fingerprint, when TLS is configured. This is the
    /// value the client pins; `status` and the installer compare against it.
    pub fn listener_fingerprint(&self) -> Option<String> {
        let cert = self.config.tls.certificate_path(&self.config.data_dir);
        let key = self.config.tls.private_key_path(&self.config.data_dir);
        if !(std::path::Path::new(&cert).exists() && std::path::Path::new(&key).exists()) {
            return None;
        }
        TlsIdentity::from_pem_files(&cert, &key)
            .ok()
            .map(|identity| identity.fingerprint_hex())
    }
}

/// Bind the loopback listener. `port == 0` asks the OS for an ephemeral port, which
/// is what the tests use; `serve` logs the port it actually got.
pub fn bind(config: &AgentConfig) -> Result<TcpListener> {
    match &config.listen {
        ListenMode::Loopback { .. } => {}
        ListenMode::SocketOnly => {
            return Err(Error::NotImplemented(
                "unix-socket listener (use `--listen 127.0.0.1:<port>` in this build)",
            ))
        }
        ListenMode::Network { .. } => {
            return Err(Error::NotImplemented(
                "network listener (mTLS); the agent is reached through the SSH tunnel",
            ))
        }
    }
    let ListenMode::Loopback { port } = config.listen else {
        return Err(Error::Invalid("listen mode disappeared".to_string()));
    };
    let listener = TcpListener::bind(("127.0.0.1", port))?;
    listener.set_nonblocking(true)?;
    Ok(listener)
}

/// Build the TLS layer, or refuse to serve.
///
/// Refusing is the default: a listener without TLS puts the device token on the wire
/// for any local process to read, and "it is only loopback" is not a trust boundary
/// (threat T3 in `docs/architecture.md` is exactly another local process).
fn tls_layer(config: &AgentConfig) -> Result<Option<Arc<ServerConfig>>> {
    if config.tls.has_identity(&config.data_dir) {
        let identity = TlsIdentity::from_pem_files(
            &config.tls.certificate_path(&config.data_dir),
            &config.tls.private_key_path(&config.data_dir),
        )?;
        return Ok(Some(crate::api::tls::server_config(&identity)?));
    }
    if config.allow_plaintext_loopback {
        return Ok(None);
    }
    Err(Error::Invalid(format!(
        "no TLS identity at {} and {}; run the installer, pass \
         --tls-cert/--tls-key, or accept plaintext explicitly with --allow-plaintext",
        config.tls.certificate_path(&config.data_dir),
        config.tls.private_key_path(&config.data_dir)
    )))
}

/// Bind and serve until `shutdown` or a termination signal.
pub fn serve(state: ServerState) -> Result<()> {
    state.config.validate()?;
    if !std::path::Path::new(&state.config.data_dir).exists() {
        return Err(Error::Invalid(format!(
            "data_dir {} does not exist; the client installer creates it",
            state.config.data_dir
        )));
    }
    let state = Arc::new(state);
    let listener = bind(&state.config)?;
    serve_on(listener, state)
}

/// [`serve`] on an already-bound listener. Split out so a test can use an ephemeral
/// port and know it before the loop starts.
pub fn serve_on(listener: TcpListener, state: Arc<ServerState>) -> Result<()> {
    let tls = tls_layer(&state.config)?;
    let local = listener.local_addr()?;
    state.logger.info(
        "api",
        &format!(
            "listening on {local} tls={} protocol={} max_connections={}",
            tls.is_some(),
            crate::PROTOCOL_VERSION,
            state.config.max_connections
        ),
    );
    if tls.is_none() {
        state.logger.warn(
            "api",
            "serving plaintext HTTP: the device token and request bodies are visible to any local process",
        );
    }
    let limiter = Arc::new(ConnectionLimiter::new(state.config.max_connections));
    crate::api::signals::install();
    let retention = spawn_retention(Arc::clone(&state));
    let db_ingest = spawn_db_ingest(Arc::clone(&state));

    let mut pending: Vec<std::thread::JoinHandle<()>> = Vec::new();
    while state.is_running() && !crate::api::signals::triggered() {
        match listener.accept() {
            Ok((stream, peer)) => {
                let Some(guard) = limiter.try_acquire() else {
                    // Over the connection limit: answer, do not queue. A queue is a
                    // slowloris amplifier; a 503 is actionable.
                    refuse_busy(stream, peer);
                    continue;
                };
                let connection_state = Arc::clone(&state);
                let tls = tls.clone();
                let handle = std::thread::Builder::new()
                    .name("tf-api-conn".to_string())
                    .spawn(move || {
                        let _guard = guard;
                        if let Err(error) =
                            serve_connection(&connection_state, stream, tls.as_ref())
                        {
                            // Never the body, never the token: the error path logs a
                            // class, not content.
                            connection_state
                                .logger
                                .warn("api", &format!("connection from {peer} ended: {error}"));
                        }
                    });
                match handle {
                    Ok(handle) => pending.push(handle),
                    Err(error) => {
                        state
                            .logger
                            .error("api", &format!("cannot start a connection thread: {error}"));
                    }
                }
                pending.retain(|handle| !handle.is_finished());
            }
            Err(error) if error.kind() == std::io::ErrorKind::WouldBlock => {
                std::thread::sleep(ACCEPT_POLL);
            }
            Err(error) if error.kind() == std::io::ErrorKind::Interrupted => {}
            Err(error) => {
                // A listener that fails to accept is not necessarily dead (EMFILE is
                // transient). Log and keep the process alive: exiting would make the
                // client's KillSwitch drop to legacy for a recoverable condition.
                state
                    .logger
                    .error("api", &format!("accept failed: {error}"));
                std::thread::sleep(ACCEPT_POLL);
            }
        }
    }

    state.shutdown();
    state
        .logger
        .info("api", "listener stopped; waiting for in-flight requests");
    for handle in pending {
        let _ = handle.join();
    }
    if let Some(retention) = retention {
        let _ = retention.join();
    }
    if let Some(db_ingest) = db_ingest {
        let _ = db_ingest.join();
    }
    state.logger.info("api", "stopped");
    Ok(())
}

fn refuse_busy(stream: TcpStream, peer: SocketAddr) {
    // The peer may be a TLS client; it will see a connection reset rather than an
    // HTTP status. That is acceptable for an overload answer and avoids doing a
    // handshake we do not have capacity for.
    let _ = peer;
    let mut stream = stream;
    let _ = http::write_response(
        &mut stream,
        &http::HttpResponse::text(503, "connection limit reached"),
    );
}

fn serve_connection(
    state: &Arc<ServerState>,
    stream: TcpStream,
    tls: Option<&Arc<ServerConfig>>,
) -> Result<()> {
    // The listener is non-blocking so the accept loop can observe shutdown; on some
    // platforms the accepted socket inherits that flag, which would turn every read
    // into WSAEWOULDBLOCK. Blocking mode plus a timeout is what the handler wants.
    stream.set_nonblocking(false)?;
    stream.set_read_timeout(Some(std::time::Duration::from_millis(
        state.config.read_timeout_ms,
    )))?;
    stream.set_write_timeout(Some(std::time::Duration::from_millis(
        state.config.write_timeout_ms,
    )))?;
    stream.set_nodelay(true)?;
    match tls {
        Some(config) => {
            let connection = rustls::ServerConnection::new(Arc::clone(config))
                .map_err(|error| Error::Tls(format!("handshake setup failed: {error}")))?;
            let mut stream = rustls::StreamOwned::new(connection, stream);
            serve_stream(state, &mut stream)
        }
        None => {
            let mut stream = stream;
            serve_stream(state, &mut stream)
        }
    }
}

/// Read exactly one request and answer it.
pub fn serve_stream<S: Read + Write>(state: &Arc<ServerState>, stream: &mut S) -> Result<()> {
    let response = {
        let mut reader = BufReader::new(&mut *stream);
        match http::read_request(&mut reader, &state.limits) {
            Ok(Some(request)) => {
                if request.path != "/" && request.path != "/rpc" {
                    if request.is_post() || request.method == "GET" {
                        Some(http::HttpResponse::text(404, "only / and /rpc exist"))
                    } else {
                        Some(http::HttpResponse::text(405, "method not allowed"))
                    }
                } else if !request.is_post() {
                    Some(http::HttpResponse::text(405, "use POST /rpc"))
                } else {
                    let response =
                        rpc::handle(state, request.header("authorization"), &request.body);
                    // Log the outcome, never the body: the body is captured metadata
                    // and ciphertext, and `redact_line` works on one line at a time.
                    state.logger.log(
                        if response.status == 200 {
                            Level::Debug
                        } else {
                            Level::Info
                        },
                        "api",
                        &format!(
                            "path={} status={} bytes={}",
                            request.path,
                            response.status,
                            request.body.len()
                        ),
                    );
                    Some(response)
                }
            }
            // Clean close between requests: nothing to answer.
            Ok(None) => None,
            Err(error) => error
                .status()
                .map(|status| http::HttpResponse::text(status, &error.message())),
        }
    };
    if let Some(response) = response {
        http::write_response(stream, &response)?;
        let _ = stream.flush();
    }
    Ok(())
}

/// Bounded connection counter.
///
/// A permit is released by `Drop`, so a panicking connection thread cannot leak a
/// slot (which would slowly reduce the agent's capacity to zero).
#[derive(Debug)]
pub struct ConnectionLimiter {
    active: AtomicUsize,
    max: usize,
}

#[derive(Debug)]
pub struct ConnectionPermit {
    limiter: Arc<ConnectionLimiter>,
}

impl ConnectionLimiter {
    pub fn new(max: usize) -> Self {
        Self {
            active: AtomicUsize::new(0),
            max: max.max(1),
        }
    }

    /// Take a slot. `self` is an `Arc` because the permit is moved into the
    /// connection thread and must keep the counter alive for its whole lifetime.
    pub fn try_acquire(self: &Arc<Self>) -> Option<ConnectionPermit> {
        let mut current = self.active.load(Ordering::SeqCst);
        loop {
            if current >= self.max {
                return None;
            }
            match self.active.compare_exchange_weak(
                current,
                current + 1,
                Ordering::SeqCst,
                Ordering::SeqCst,
            ) {
                Ok(_) => {
                    return Some(ConnectionPermit {
                        limiter: Arc::clone(self),
                    })
                }
                Err(actual) => current = actual,
            }
        }
    }

    pub fn active(&self) -> usize {
        self.active.load(Ordering::SeqCst)
    }

    pub fn max(&self) -> usize {
        self.max
    }
}

impl Drop for ConnectionPermit {
    fn drop(&mut self) {
        self.limiter.active.fetch_sub(1, Ordering::SeqCst);
    }
}

/// Start the periodic retention task. Returns `None` when the interval is 0 (the
/// scheduler is disabled, the functions remain callable).
pub fn spawn_retention(state: Arc<ServerState>) -> Option<std::thread::JoinHandle<()>> {
    let interval_secs = state.config.retention_interval_secs;
    if interval_secs == 0 {
        state.logger.info(
            "retention",
            "scheduler disabled (retention_interval_secs = 0)",
        );
        return None;
    }
    let handle = std::thread::Builder::new()
        .name("tf-retention".to_string())
        .spawn(move || {
            let interval = std::time::Duration::from_secs(interval_secs);
            let mut elapsed = std::time::Duration::ZERO;
            while state.is_running() {
                // Sleep in slices so a stop request is observed within `ACCEPT_POLL`
                // instead of after the whole interval.
                std::thread::sleep(ACCEPT_POLL);
                if !state.is_running() {
                    break;
                }
                elapsed += ACCEPT_POLL;
                if elapsed < interval {
                    continue;
                }
                elapsed = std::time::Duration::ZERO;
                run_retention_once(&state);
            }
        })
        .ok()?;
    Some(handle)
}

/// One retention pass, with logging and without propagating errors.
///
/// A retention failure is never fatal: stopping the process because a tenant's file
/// is locked would take the sync endpoint down for a maintenance task. The failure is
/// logged (table counts, tenant id) and retried on the next interval.
pub fn run_retention_once(state: &Arc<ServerState>) {
    let now = SystemClock.now_ms();
    let now_ms = i64::try_from(now).unwrap_or(i64::MAX);
    let reports = state.sync.run_retention(now_ms);
    if reports.is_empty() {
        state
            .logger
            .log(Level::Debug, "retention", "no tenants to purge");
        return;
    }
    for (tenant, report) in reports {
        match report {
            Ok(report) if report.is_empty() => state.logger.log(
                Level::Debug,
                "retention",
                &format!("tenant {tenant}: nothing to purge"),
            ),
            Ok(report) => state.logger.info(
                "retention",
                &format!("tenant {tenant}: purged {}", report.to_json()),
            ),
            Err(error) => state.logger.error(
                "retention",
                &format!("tenant {tenant}: purge failed: {error}"),
            ),
        }
    }
}

/// Start the periodic DB ingest. Returns `None` when the interval is `0` (the pass
/// stays callable through `db.ingest`) or when the DB path is not configured.
///
/// Same shape as [`spawn_retention`] on purpose: one thread, sleeps in `ACCEPT_POLL`
/// slices so a stop request is observed within the poll interval, and **no error
/// escapes**: a failed pass is logged as a class and retried on the next tick with the
/// cursor untouched (`docs/db-ingest.md` § 5).
pub fn spawn_db_ingest(state: Arc<ServerState>) -> Option<std::thread::JoinHandle<()>> {
    let ingest = state.db.as_ref()?;
    let interval_secs = ingest.interval_secs();
    if interval_secs == 0 {
        state.logger.info(
            "db-ingest",
            "scheduler disabled (db ingest interval = 0); db.status and db.ingest still work",
        );
        return None;
    }
    let handle = std::thread::Builder::new()
        .name("tf-db-ingest".to_string())
        .spawn(move || {
            let interval = std::time::Duration::from_secs(interval_secs);
            let mut elapsed = std::time::Duration::ZERO;
            while state.is_running() {
                std::thread::sleep(ACCEPT_POLL);
                if !state.is_running() {
                    break;
                }
                elapsed += ACCEPT_POLL;
                if elapsed < interval {
                    continue;
                }
                elapsed = std::time::Duration::ZERO;
                run_db_ingest_once(&state);
            }
        })
        .ok()?;
    Some(handle)
}

/// One DB ingest pass, with logging and without propagating errors.
///
/// The vault is unlocked only in-process (the `unlock` verb over the wire is a
/// separate step), so a pass on a locked vault returns `Error::Auth`. That is not a
/// failure of the *process*: the pass is logged with its class (`vault_locked`),
/// `db.status` reports `vault: "locked"` + the same class, and `sync.pull` keeps
/// serving the journal.
pub fn run_db_ingest_once(state: &Arc<ServerState>) {
    let Some(ingest) = state.db.as_ref() else {
        return;
    };
    let tenant = ingest.config().tenant.clone();
    let now_ms = i64::try_from(SystemClock.now_ms()).unwrap_or(i64::MAX);
    let result = state.sync.with_store(&tenant, |store| {
        crate::db::ingest::run_and_log(ingest, store, now_ms)
    });
    // The pass itself already logged its outcome; this only distinguishes "the store
    // per tenant could not even be opened" from "the pass failed", and never prints a
    // value.
    if let Err(error) = result {
        state.logger.error(
            "db-ingest",
            &format!("tenant {tenant}: {}", crate::db::class_of(&error)),
        );
    }
}

/// Idempotency store. Bounded and time-limited, because an unbounded map of/// request ids is a memory leak — the same failure as `g_tasks` in
/// `plugin_runner.cpp` (finding L5).
#[derive(Debug)]
pub struct IdempotencyStore {
    entries: std::collections::BTreeMap<String, StoredResponse>,
    capacity: usize,
    ttl_ms: u64,
}

#[derive(Clone, Debug)]
pub struct StoredResponse {
    pub body: String,
    pub stored_at_ms: u64,
}

impl IdempotencyStore {
    pub fn new(capacity: usize, ttl_ms: u64) -> Self {
        Self {
            entries: std::collections::BTreeMap::new(),
            capacity: capacity.max(1),
            ttl_ms,
        }
    }

    pub fn get(&self, key: &str, now_ms: u64) -> Option<&StoredResponse> {
        self.entries
            .get(key)
            .filter(|stored| now_ms.saturating_sub(stored.stored_at_ms) <= self.ttl_ms)
    }

    pub fn put(&mut self, key: &str, body: &str, now_ms: u64) {
        self.evict(now_ms);
        if self.entries.len() >= self.capacity {
            // Evict the oldest by stored_at, not by key order: the key is a ULID
            // so key order *is* time order today, but relying on that would break
            // the moment a client sends a UUID.
            if let Some(oldest) = self
                .entries
                .iter()
                .min_by_key(|(_, stored)| stored.stored_at_ms)
                .map(|(key, _)| key.clone())
            {
                self.entries.remove(&oldest);
            }
        }
        self.entries.insert(
            key.to_string(),
            StoredResponse {
                body: body.to_string(),
                stored_at_ms: now_ms,
            },
        );
    }

    pub fn evict(&mut self, now_ms: u64) {
        let ttl = self.ttl_ms;
        self.entries
            .retain(|_, stored| now_ms.saturating_sub(stored.stored_at_ms) <= ttl);
    }

    pub fn len(&self) -> usize {
        self.entries.len()
    }

    pub fn is_empty(&self) -> bool {
        self.entries.is_empty()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::api::base64;
    use crate::api::tls::test_identity;
    use crate::auth::Scope;
    use crate::store::AccountId;
    use crate::test_support::TestDir;
    use serde_json::{json, Value};

    const TENANT: &str = "00112233445566778899aabbccddeeff";

    fn account() -> AccountId {
        AccountId::new([0x71; 16]).unwrap()
    }

    /// A server with TLS, one enrolled device, bound to an ephemeral port.
    struct Harness {
        _directory: TestDir,
        state: Arc<ServerState>,
        token: String,
        device: String,
        account: AccountId,
        address: SocketAddr,
        identity: crate::api::tls::TlsIdentity,
        thread: Option<std::thread::JoinHandle<()>>,
        /// Tenant DEK of the DB-ingest fixture, when the harness was started with one.
        dek: Option<crate::secret::SecretBytes>,
    }

    impl Harness {
        fn start(account: AccountId) -> Self {
            Self::start_with(account, None)
        }

        /// [`Self::start`] plus a configured, unlocked DB ingest backed by
        /// [`crate::db::MockSource`]: the seam that lets the server-side cycle be
        /// tested over TLS without a live PostgreSQL.
        fn start_with_db(account: AccountId, rows: Vec<crate::db::DbRow>) -> Self {
            Self::start_with(account, Some(rows))
        }

        fn start_with(account: AccountId, db_rows: Option<Vec<crate::db::DbRow>>) -> Self {
            let directory = TestDir::new("api");
            let data_dir = directory.path().to_string_lossy().to_string();
            let identity = test_identity::identity();
            // Write the identity exactly where the installer puts it, so the test
            // exercises the real PEM parsing path instead of injecting DER.
            let identity_dir = directory.join("identity");
            std::fs::create_dir_all(&identity_dir).unwrap();
            std::fs::write(
                identity_dir.join("agent.cert.pem"),
                pem("CERTIFICATE", &identity.cert_der),
            )
            .unwrap();
            std::fs::write(
                identity_dir.join("agent.key.pem"),
                pem("PRIVATE KEY", &identity.key_der),
            )
            .unwrap();

            let config = crate::config::AgentConfig {
                data_dir: data_dir.clone(),
                listen: ListenMode::Loopback { port: 0 },
                rate_limit_burst: 120,
                rate_limit_per_sec: 50,
                retention_interval_secs: 0,
                db_ingest: crate::config::DbIngestSettings {
                    tenant: Some(TENANT.to_string()),
                    account: Some(account.to_hex()),
                    ..Default::default()
                },
                ..Default::default()
            };

            let mut state = ServerState::new(config).unwrap();
            let (token, record) = state
                .authorizer
                .get_mut()
                .unwrap()
                .enroll(
                    "test-device",
                    vec![Scope::SyncRead, Scope::SyncWrite, Scope::Status],
                    Some(TENANT.to_string()),
                    Some(account),
                    1_000,
                )
                .unwrap();
            let token_hex = crate::crypto::sig::to_hex(token.expose());
            let device = record.device_id_hex();

            let mut dek = None;
            if let Some(rows) = db_rows {
                let key = crate::secret::SecretBytes::new(vec![0x4c; 32]);
                crate::db::credentials::store(
                    &data_dir,
                    TENANT,
                    &key,
                    1,
                    &crate::secret::SecretBytes::new(
                        b"postgresql://teleforge_agent:hunter2@db.example.com:5432/teleforge"
                            .to_vec(),
                    ),
                )
                .unwrap();
                let ingest = crate::db::DbIngest::with_connector(
                    crate::db::DbIngestConfig::new(data_dir.clone(), TENANT, account),
                    crate::logging::Logger::new(
                        crate::logging::Level::Error,
                        crate::logging::Destination::Stderr,
                    ),
                    Box::new(move |_facts, _conn, _spec| {
                        Ok(Box::new(crate::db::MockSource::new(rows.clone())))
                    }),
                )
                .unwrap();
                ingest.unlock(&key, 1).unwrap();
                state.db = Some(Arc::new(ingest));
                dek = Some(key);
            }

            let state = Arc::new(state);
            let listener = bind(&state.config).unwrap();
            let address = listener.local_addr().unwrap();
            let to_serve = Arc::clone(&state);
            let thread = std::thread::spawn(move || {
                let _ = serve_on(listener, to_serve);
            });
            // Wait for the listener to answer (the accept loop starts within one
            // poll interval).
            Self {
                _directory: directory,
                state,
                token: token_hex,
                device,
                account,
                address,
                identity,
                thread: Some(thread),
                dek,
            }
        }

        fn data_dir(&self) -> String {
            self._directory.path().to_string_lossy().to_string()
        }

        fn dek(&self) -> &crate::secret::SecretBytes {
            self.dek
                .as_ref()
                .expect("this harness was not started with a DB fixture")
        }

        fn client(&self) -> Client<'_> {
            Client::connect(self.address, &self.identity, Some(&self.token))
        }

        fn client_with<'a>(&'a self, token: &'a str) -> Client<'a> {
            Client::connect(self.address, &self.identity, Some(token))
        }

        fn anonymous(&self) -> Client<'_> {
            Client::connect(self.address, &self.identity, None)
        }

        /// Enroll a second device whose role is `ingest` (§ 15 п. 11) and return its
        /// token.
        fn enroll_ingest_device(&self) -> String {
            let (token, _record) = self
                .state
                .authorizer
                .lock()
                .expect("the auth store is not poisoned")
                .enroll_with_role(
                    "ingest-device",
                    crate::auth::TokenRole::Ingest,
                    vec![Scope::Status, Scope::SyncRead, Scope::DbIngest],
                    Some(TENANT.to_string()),
                    Some(self.account),
                    1_000,
                )
                .expect("an ingest token can be enrolled");
            crate::crypto::sig::to_hex(token.expose())
        }

        fn stop(mut self) {
            self.state.shutdown();
            if let Some(thread) = self.thread.take() {
                let _ = thread.join();
            }
        }
    }

    /// `tenant` + `account` of the fixture, as every method that needs them expects.
    fn db_params(account: AccountId) -> Value {
        json!({"tenant": TENANT, "account": account.to_hex()})
    }

    impl Drop for Harness {
        fn drop(&mut self) {
            self.state.shutdown();
            if let Some(thread) = self.thread.take() {
                let _ = thread.join();
            }
        }
    }

    fn pem(label: &str, der: &[u8]) -> String {
        let body = base64::encode(der);
        let mut out = format!("-----BEGIN {label}-----\n");
        for chunk in body.as_bytes().chunks(64) {
            out.push_str(std::str::from_utf8(chunk).unwrap());
            out.push('\n');
        }
        out.push_str(&format!("-----END {label}-----\n"));
        out
    }

    /// A test client that opens **one connection per request**, mirroring the
    /// server's `Connection: close` contract. Reusing a stream would test a
    /// keep-alive path that deliberately does not exist.
    struct Client<'a> {
        address: SocketAddr,
        cert_der: Vec<u8>,
        token: Option<&'a str>,
    }

    impl<'a> Client<'a> {
        fn connect(
            address: SocketAddr,
            identity: &crate::api::tls::TlsIdentity,
            token: Option<&'a str>,
        ) -> Self {
            Self {
                address,
                cert_der: identity.cert_der.clone(),
                token,
            }
        }

        fn open(&self) -> rustls::StreamOwned<rustls::ClientConnection, TcpStream> {
            let config = crate::api::tls::pinned_client_config(&self.cert_der).unwrap();
            let server_name = rustls::pki_types::ServerName::try_from("localhost").unwrap();
            let tcp = TcpStream::connect(self.address).unwrap();
            tcp.set_read_timeout(Some(std::time::Duration::from_secs(10)))
                .unwrap();
            tcp.set_write_timeout(Some(std::time::Duration::from_secs(10)))
                .unwrap();
            let connection = rustls::ClientConnection::new(config, server_name).unwrap();
            rustls::StreamOwned::new(connection, tcp)
        }

        fn raw(&self, head: &str, body: &[u8]) -> (u16, String) {
            let mut stream = self.open();
            let mut request = head.as_bytes().to_vec();
            request.extend_from_slice(body);
            stream.write_all(&request).unwrap();
            stream.flush().unwrap();
            read_response(&mut stream)
        }

        fn call(&self, method: &str, params: Value, idempotency: Option<&str>) -> (u16, Value) {
            let body = json!({
                "protocol": 1,
                "request_id": format!("req-{method}"),
                "idempotency_key": idempotency.unwrap_or(""),
                "method": method,
                "params": params,
            })
            .to_string();
            let mut head = format!(
                "POST /rpc HTTP/1.1\r\nHost: localhost\r\nContent-Length: {}\r\n",
                body.len()
            );
            if let Some(token) = self.token {
                head.push_str(&format!("Authorization: Bearer {token}\r\n"));
            }
            head.push_str("\r\n");
            let (status, text) = self.raw(&head, body.as_bytes());
            let parsed: Value =
                serde_json::from_str(&text).unwrap_or_else(|_| json!({"parse_error": text}));
            (status, parsed)
        }

        fn call_direct(&self, body: &[u8]) -> (u16, Value) {
            let head = format!(
                "POST /rpc HTTP/1.1\r\nHost: localhost\r\nContent-Length: {}\r\n\r\n",
                body.len()
            );
            let (status, text) = self.raw(&head, body);
            let parsed: Value =
                serde_json::from_str(&text).unwrap_or_else(|_| json!({"parse_error": text}));
            (status, parsed)
        }
    }
    fn read_response(stream: &mut impl Read) -> (u16, String) {
        let mut head = Vec::new();
        let mut byte = [0u8; 1];
        loop {
            match stream.read(&mut byte) {
                Ok(0) => break,
                Ok(_) => {
                    head.push(byte[0]);
                    if head.ends_with(b"\r\n\r\n") {
                        break;
                    }
                }
                Err(error) => panic!("cannot read the response head: {error}"),
            }
        }
        let head = String::from_utf8(head).expect("ascii head");
        let status: u16 = head
            .split(' ')
            .nth(1)
            .and_then(|text| text.parse().ok())
            .expect("status code");
        let length: usize = head
            .lines()
            .find_map(|line| {
                let (name, value) = line.split_once(':')?;
                if name.eq_ignore_ascii_case("content-length") {
                    value.trim().parse().ok()
                } else {
                    None
                }
            })
            .unwrap_or(0);
        let mut body = vec![0u8; length];
        if length > 0 {
            stream.read_exact(&mut body).expect("response body");
        }
        (status, String::from_utf8(body).expect("utf-8 response"))
    }

    fn op(kind: &str, entity: &str, field: Option<&str>, value: &[u8], hlc: &str) -> Value {
        let mut object = json!({
            "kind": kind,
            "entity_id": entity,
            "hlc": hlc,
            "value": base64::encode(value),
        });
        if let Some(field) = field {
            object["field"] = json!(field);
        }
        object
    }

    fn address_hlc(physical: u64) -> String {
        format!("{physical:016x}-0000")
    }

    #[test]
    fn hello_is_answered_without_a_token() {
        let harness = Harness::start(account());
        let client = harness.anonymous();
        let (status, response) = client.call("hello", json!({}), None);
        assert_eq!(status, 200);
        assert_eq!(response["ok"], true);
        assert_eq!(response["result"]["agent_version"], crate::AGENT_VERSION);
        assert_eq!(response["result"]["protocol"], 1);
        assert_eq!(response["error"], Value::Null);
        harness.stop();
    }

    #[test]
    fn a_missing_or_wrong_token_is_401() {
        let harness = Harness::start(account());
        let anonymous = harness.anonymous();
        let (status, response) = anonymous.call(
            "sync.stamp",
            json!({"tenant": TENANT, "account": account().to_hex()}),
            None,
        );
        assert_eq!(status, 401);
        assert_eq!(response["error"]["code"], 1002);

        let bad_token = "ab".repeat(32);
        let wrong = Client::connect(harness.address, &harness.identity, Some(&bad_token));
        let (status, response) = wrong.call(
            "sync.stamp",
            json!({"tenant": TENANT, "account": account().to_hex()}),
            None,
        );
        assert_eq!(status, 401);
        assert_eq!(response["error"]["code"], 1002);
        harness.stop();
    }

    #[test]
    fn a_foreign_account_is_403_before_anything_is_parsed() {
        let harness = Harness::start(account());
        let client = harness.client();
        let other = AccountId::new([0x99; 16]).unwrap().to_hex();
        let (status, response) = client.call(
            "sync.push",
            json!({"tenant": TENANT, "account": other, "ops": []}),
            Some("idem-foreign"),
        );
        assert_eq!(status, 403);
        assert_eq!(response["error"]["code"], 1003);
        assert!(response["error"]["message"]
            .as_str()
            .unwrap()
            .contains("enrolled for account"));

        // Another tenant is refused the same way.
        let (status, _) = client.call(
            "sync.stamp",
            json!({"tenant": "ffeeddccbbaa99887766554433221100", "account": account().to_hex()}),
            None,
        );
        assert_eq!(status, 403);
        harness.stop();
    }

    #[test]
    fn push_then_pull_roundtrips_over_tls() {
        let harness = Harness::start(account());
        let client = harness.client();
        let ops = vec![
            op(
                "message_new",
                "message:-100:42",
                Some("text"),
                b"hello",
                &address_hlc(1_000),
            ),
            op(
                "message_edit",
                "message:-100:42",
                Some("text"),
                b"hello edited",
                &address_hlc(1_001),
            ),
            op(
                "presence_online",
                "presence:900",
                None,
                &5_000i64.to_be_bytes(),
                &address_hlc(1_002),
            ),
        ];
        let (status, pushed) = client.call(
            "sync.push",
            json!({"tenant": TENANT, "account": account().to_hex(), "ops": ops}),
            Some("push-1"),
        );
        assert_eq!(status, 200, "{pushed}");
        assert_eq!(pushed["result"]["applied"], 3);
        assert_eq!(pushed["result"]["duplicates"], 0);
        assert_eq!(pushed["result"]["rejected"], 0);
        assert_eq!(pushed["result"]["high_water"], 3);
        assert_eq!(pushed["result"]["results"][0]["verdict"], "applied");
        assert_eq!(pushed["result"]["results"][0]["op_seq"], 1);
        assert_eq!(pushed["result"]["account_stamp"]["op_count"], 3);
        // The device id is the token's device, not something the client chose.
        assert_eq!(pushed["result"]["account_stamp"]["high_water"], 3);

        let (status, pulled) = client.call(
            "sync.pull",
            json!({"tenant": TENANT, "account": account().to_hex(), "since_seq": 0, "limit": 10}),
            None,
        );
        assert_eq!(status, 200, "{pulled}");
        let result = &pulled["result"];
        assert_eq!(result["ops"].as_array().unwrap().len(), 3);
        assert_eq!(result["next_seq"], 3);
        assert_eq!(result["has_more"], false);
        assert_eq!(result["ops"][1]["kind_name"], "message_edit");
        assert_eq!(
            result["ops"][1]["value"],
            base64::encode(b"hello edited"),
            "the value survives the round trip byte for byte"
        );
        assert_eq!(result["ops"][0]["device_id"], harness.device);
        assert_eq!(result["ops"][0]["hlc"], address_hlc(1_000));

        // Pulling from the end is empty, not an error.
        let (status, empty) = client.call(
            "sync.pull",
            json!({"tenant": TENANT, "account": account().to_hex(), "since_seq": 3}),
            None,
        );
        assert_eq!(status, 200);
        assert!(empty["result"]["ops"].as_array().unwrap().is_empty());
        harness.stop();
    }

    #[test]
    fn replaying_the_same_push_is_idempotent() {
        let harness = Harness::start(account());
        let client = harness.client();
        let ops = vec![op(
            "message_new",
            "message:-100:42",
            Some("text"),
            b"once",
            &address_hlc(1_000),
        )];
        let params = json!({"tenant": TENANT, "account": account().to_hex(), "ops": ops});
        let (status, first) = client.call("sync.push", params.clone(), Some("push-1"));
        assert_eq!(status, 200);
        assert_eq!(first["result"]["applied"], 1);

        // Same idempotency key: the stored answer is replayed verbatim.
        let (status, replay) = client.call("sync.push", params.clone(), Some("push-1"));
        assert_eq!(status, 200);
        assert_eq!(replay, first);

        // A different key, same batch: the store dedupes by content hash.
        let (status, second) = client.call("sync.push", params, Some("push-2"));
        assert_eq!(status, 200);
        assert_eq!(second["result"]["applied"], 0);
        assert_eq!(second["result"]["duplicates"], 1);
        assert_eq!(second["result"]["high_water"], 1);

        let (_, stamp) = client.call(
            "sync.stamp",
            json!({"tenant": TENANT, "account": account().to_hex()}),
            None,
        );
        assert_eq!(stamp["result"]["op_count"], 1);
        harness.stop();
    }

    #[test]
    fn pagination_over_the_wire_is_stable() {
        let harness = Harness::start(account());
        let client = harness.client();
        let ops: Vec<Value> = (0..5)
            .map(|index| {
                op(
                    "message_new",
                    &format!("message:-100:{index}"),
                    Some("text"),
                    b"x",
                    &address_hlc(1_000 + index),
                )
            })
            .collect();
        let (status, _) = client.call(
            "sync.push",
            json!({"tenant": TENANT, "account": account().to_hex(), "ops": ops}),
            Some("push-page"),
        );
        assert_eq!(status, 200);

        let mut cursor = 0u64;
        let mut seen = 0usize;
        for _ in 0..4 {
            let (status, page) = client.call(
                "sync.pull",
                json!({"tenant": TENANT, "account": account().to_hex(),
                       "since_seq": cursor, "limit": 2}),
                None,
            );
            assert_eq!(status, 200);
            let result = &page["result"];
            let count = result["ops"].as_array().unwrap().len();
            seen += count;
            cursor = result["next_seq"].as_u64().unwrap();
            if !result["has_more"].as_bool().unwrap() {
                break;
            }
            assert_eq!(count, 2);
        }
        assert_eq!(seen, 5);
        assert_eq!(cursor, 5);
        harness.stop();
    }

    #[test]
    fn a_bad_body_is_400_and_a_bad_cursor_is_409() {
        let harness = Harness::start(account());
        let client = harness.client();

        let (status, response) = client.call_direct(b"{not json");
        assert_eq!(status, 400);
        assert_eq!(response["error"]["code"], 1000);

        let (status, response) = client.call_direct(br#"{"request_id":"r","method":"nope"}"#);
        assert_eq!(status, 400);
        assert!(response["error"]["message"]
            .as_str()
            .unwrap()
            .contains("unknown method"));

        let (status, response) = client.call(
            "sync.pull",
            json!({"tenant": TENANT, "account": account().to_hex(), "since_seq": 99}),
            None,
        );
        assert_eq!(status, 409);
        assert_eq!(response["error"]["code"], 1006);

        // A mutating call without an idempotency key is a 400, not a silent retry
        // hazard.
        let (status, response) = client.call(
            "sync.push",
            json!({"tenant": TENANT, "account": account().to_hex(), "ops": []}),
            None,
        );
        assert_eq!(status, 400);
        assert!(response["error"]["message"]
            .as_str()
            .unwrap()
            .contains("idempotency_key"));
        harness.stop();
    }

    #[test]
    fn an_oversized_body_is_413_before_it_is_read() {
        let harness = Harness::start(account());
        let client = harness.client();
        let head = "POST /rpc HTTP/1.1\r\nHost: localhost\r\nContent-Length: 99999999\r\n\r\n";
        let (status, body) = client.raw(head, b"");
        assert_eq!(status, 413);
        assert!(body.contains("exceeds"));
        harness.stop();
    }

    #[test]
    fn conflicts_and_prune_work_over_the_wire() {
        let harness = Harness::start(account());
        let client = harness.client();
        let newer = op(
            "profile_update",
            "profile:900",
            Some("username"),
            b"@new",
            &address_hlc(2_000),
        );
        let (status, _) = client.call(
            "sync.push",
            json!({"tenant": TENANT, "account": account().to_hex(), "ops": [newer]}),
            Some("push-newer"),
        );
        assert_eq!(status, 200);
        // A conflict is *cross-device* by definition: the second write must come from
        // another enrolled device, so enroll one here.
        let (second_token, _) = harness
            .state
            .authorizer
            .lock()
            .unwrap()
            .enroll(
                "second-device",
                vec![Scope::SyncRead, Scope::SyncWrite],
                Some(TENANT.to_string()),
                Some(account()),
                2_000,
            )
            .unwrap();
        let second_token_hex = crate::crypto::sig::to_hex(second_token.expose());
        let second = Client::connect(harness.address, &harness.identity, Some(&second_token_hex));
        let older = op(
            "profile_update",
            "profile:900",
            Some("username"),
            b"@old",
            &address_hlc(1_000),
        );
        let (status, pushed) = second.call(
            "sync.push",
            json!({"tenant": TENANT, "account": account().to_hex(), "ops": [older]}),
            Some("push-older"),
        );
        assert_eq!(status, 200);
        assert_eq!(pushed["result"]["conflicts"], 1);
        assert_eq!(pushed["result"]["conflict_ids"][0], 1);

        let (status, listed) = client.call(
            "sync.conflicts",
            json!({"tenant": TENANT, "account": account().to_hex(),
                   "resolve": [{"conflict_id": 1, "resolved_by": "tester"}]}),
            Some("conflicts-1"),
        );
        assert_eq!(status, 200, "{listed}");
        assert_eq!(listed["result"]["resolved"], 1);
        assert_eq!(listed["result"]["conflicts"][0]["conflict_id"], 1);
        assert_eq!(listed["result"]["conflicts"][0]["resolved_by"], "tester");

        // Prune up to the first op; the cursor then reports the floor.
        let (status, pruned) = client.call(
            "sync.prune",
            json!({"tenant": TENANT, "account": account().to_hex(), "upto_seq": 1}),
            Some("prune-1"),
        );
        assert_eq!(status, 200, "{pruned}");
        assert_eq!(pruned["result"]["pruned"]["ops"], 1);
        assert_eq!(pruned["result"]["high_water"], 2);

        let (status, stale) = client.call(
            "sync.pull",
            json!({"tenant": TENANT, "account": account().to_hex(), "since_seq": 0}),
            None,
        );
        assert_eq!(status, 409, "a pruned cursor must not look up to date");
        assert!(stale["error"]["message"]
            .as_str()
            .unwrap()
            .contains("pruned"));

        // Pruning beyond high_water is refused.
        let (status, too_far) = client.call(
            "sync.prune",
            json!({"tenant": TENANT, "account": account().to_hex(), "upto_seq": 99}),
            Some("prune-2"),
        );
        assert_eq!(status, 409);
        assert_eq!(too_far["error"]["code"], 1006);
        harness.stop();
    }

    #[test]
    fn the_rate_limit_answers_429() {
        let directory = TestDir::new("api-rate");
        let data_dir = directory.path().to_string_lossy().to_string();
        let identity = test_identity::identity();
        let identity_dir = directory.join("identity");
        std::fs::create_dir_all(&identity_dir).unwrap();
        std::fs::write(
            identity_dir.join("agent.cert.pem"),
            pem("CERTIFICATE", &identity.cert_der),
        )
        .unwrap();
        std::fs::write(
            identity_dir.join("agent.key.pem"),
            pem("PRIVATE KEY", &identity.key_der),
        )
        .unwrap();
        // One request, then nothing for a second: the second call must be refused.
        let config = crate::config::AgentConfig {
            data_dir,
            listen: ListenMode::Loopback { port: 0 },
            retention_interval_secs: 0,
            rate_limit_burst: 1,
            rate_limit_per_sec: 1,
            ..Default::default()
        };
        let mut state = ServerState::new(config).unwrap();
        let (token, _) = state
            .authorizer
            .get_mut()
            .unwrap()
            .enroll(
                "rate",
                vec![Scope::Status, Scope::SyncRead],
                Some(TENANT.to_string()),
                Some(account()),
                1,
            )
            .unwrap();
        let token_hex = crate::crypto::sig::to_hex(token.expose());
        let state = Arc::new(state);
        let listener = bind(&state.config).unwrap();
        let address = listener.local_addr().unwrap();
        let to_serve = Arc::clone(&state);
        let thread = std::thread::spawn(move || {
            let _ = serve_on(listener, to_serve);
        });
        // Two requests must land inside the same refill window; otherwise the
        // second one legitimately gets a fresh token and the assertion becomes a
        // race against the clock. Firing them concurrently keeps the window at
        // milliseconds instead of relying on the test being faster than one second.
        let fired: Vec<_> = (0..2)
            .map(|_| {
                let token = token_hex.clone();
                let identity = identity.clone();
                std::thread::spawn(move || {
                    let client = Client::connect(address, &identity, Some(&token));
                    client.call(
                        "sync.stamp",
                        json!({"tenant": TENANT, "account": account().to_hex()}),
                        None,
                    )
                })
            })
            .collect();
        let responses: Vec<(u16, Value)> = fired
            .into_iter()
            .map(|handle| handle.join().expect("request thread"))
            .collect();
        let mut statuses: Vec<u16> = responses.iter().map(|(status, _)| *status).collect();
        statuses.sort_unstable();
        assert_eq!(
            statuses,
            vec![200, 429],
            "exactly one request may pass a burst of one: {responses:?}"
        );
        let refused = responses
            .iter()
            .find(|(status, _)| *status == 429)
            .expect("one refusal");
        assert_eq!(refused.1["error"]["code"], 1007);
        state.shutdown();
        let _ = thread.join();
    }

    #[test]
    fn the_connection_limiter_is_bounded_and_releases_permits() {
        let limiter = Arc::new(ConnectionLimiter::new(2));
        assert_eq!(limiter.max(), 2);
        let first = limiter.try_acquire().expect("first permit");
        let second = limiter.try_acquire().expect("second permit");
        assert!(limiter.try_acquire().is_none(), "the third is refused");
        assert_eq!(limiter.active(), 2);
        drop(first);
        assert_eq!(limiter.active(), 1);
        let third = limiter.try_acquire().expect("a freed slot is reusable");
        drop(third);
        drop(second);
        assert_eq!(limiter.active(), 0);
    }

    #[test]
    fn tls_is_refused_without_an_identity_unless_explicitly_allowed() {
        let directory = TestDir::new("api-tls");
        let config = crate::config::AgentConfig {
            data_dir: directory.path().to_string_lossy().to_string(),
            listen: ListenMode::Loopback { port: 0 },
            ..Default::default()
        };
        let error = tls_layer(&config).unwrap_err();
        assert!(error.to_string().contains("no TLS identity"), "{error}");

        let config = crate::config::AgentConfig {
            allow_plaintext_loopback: true,
            ..config
        };
        assert!(tls_layer(&config).unwrap().is_none());
    }

    #[test]
    fn db_status_requires_a_token_on_the_wire() {
        let harness = Harness::start_with_db(account(), Vec::new());
        // No token: 401, before anything is parsed.
        let (status, body) = harness
            .anonymous()
            .call("db.status", db_params(account()), None);
        assert_eq!(status, 401);
        assert_eq!(body["error"]["code"], 1002);
        harness.stop();
    }

    #[test]
    fn db_status_answers_with_the_contract_fields() {
        let account = account();
        let harness = Harness::start_with_db(
            account,
            vec![crate::db::tests_row(1, "message:1:1", "text", b"hello")],
        );
        let (status, body) = harness.client().call("db.status", db_params(account), None);
        assert_eq!(status, 200, "{body}");
        let result = &body["result"];
        // Exactly the § 5.8 fields, present even when they are null.
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
        ] {
            assert!(result.get(field).is_some(), "db.status needs `{field}`");
        }
        assert_eq!(result["connected"], true);
        assert_eq!(result["tables_ok"], true);
        assert_eq!(result["engine"], "postgresql");
        assert_eq!(result["database"], "teleforge");
        assert_eq!(result["reader_user"], "teleforge_agent");
        assert_eq!(result["vault"], "unlocked");
        assert!(result["last_error"].is_null());
        // Before the first pass there is no cursor yet, and the field says so.
        assert!(result["cursor"].is_null());
        // The connection string and the password are nowhere in the answer.
        let text = body.to_string();
        assert!(!text.contains("hunter2"), "the password leaked: {text}");
        assert!(!text.contains("postgresql://"), "the URL leaked: {text}");
        harness.stop();
    }

    #[test]
    fn db_status_reports_a_locked_vault_without_failing() {
        let account = account();
        let harness = Harness::start_with_db(account, Vec::new());
        // Lock the vault after the harness unlocked it.
        harness
            .state
            .db
            .as_ref()
            .expect("the DB path is configured")
            .lock();
        let (status, body) = harness.client().call("db.status", db_params(account), None);
        assert_eq!(status, 200, "a locked vault is a state, not an error");
        assert_eq!(body["result"]["vault"], "locked");
        assert_eq!(body["result"]["connected"], false);
        assert_eq!(body["result"]["last_error"], "vault_locked");
        // `sync.pull` keeps working: the journal is local and holds ciphertext.
        let (pull_status, _) = harness.client().call(
            "sync.pull",
            json!({"tenant": TENANT, "account": account.to_hex(), "since_seq": 0}),
            None,
        );
        assert_eq!(pull_status, 200);
        harness.stop();
    }

    #[test]
    fn the_full_server_side_cycle_reaches_sync_pull() {
        // The pull-first contract, end to end over TLS:
        // DB rows → `db.ingest` → sealed `ops` → `sync.pull` → the client.
        let account = account();
        let harness = Harness::start_with_db(
            account,
            vec![
                crate::db::tests_row(1, "message:1:1", "text", b"hello"),
                crate::db::tests_row(2, "message:1:2", "text", b"world"),
            ],
        );
        let ingest_token = harness.enroll_ingest_device();
        let (status, body) = harness.client_with(&ingest_token).call(
            "db.ingest",
            db_params(account),
            Some("idem-db-ingest-1"),
        );
        assert_eq!(status, 200, "{body}");
        assert_eq!(body["result"]["pass"]["rows"], 2);
        assert_eq!(body["result"]["pass"]["applied"], 2);
        assert_eq!(body["result"]["pass"]["cursor"], 2);

        let (status, body) = harness.client().call(
            "sync.pull",
            json!({"tenant": TENANT, "account": account.to_hex(), "since_seq": 0}),
            None,
        );
        assert_eq!(status, 200);
        let ops = body["result"]["ops"]
            .as_array()
            .expect("ops must be an array");
        assert_eq!(ops.len(), 2);
        assert_eq!(ops[0]["device_id"], crate::db::SERVER_DEVICE_ID);
        assert_eq!(ops[0]["entity_id"], "message:1:1");
        assert_eq!(ops[0]["field"], "text");
        // The value the client receives is ciphertext, and it opens with the key the
        // contract names.
        let value =
            crate::api::base64::decode(ops[0]["value"].as_str().expect("value is a string"))
                .expect("the value is base64");
        let sync_key =
            crate::crypto::kdf::subkey(harness.dek(), crate::crypto::kdf::LABEL_SYNC_DATA)
                .expect("a subkey can be derived");
        let plain = crate::crypto::aead::open(
            &sync_key,
            &crate::db::ingest::value_aad(
                crate::store::EventKind::MessageNew,
                "message:1:1",
                Some("text"),
            ),
            &value,
        )
        .expect("the pulled value must open with the sync subkey");
        assert_eq!(plain.expose(), b"hello");

        // ... and `db.status` now reports the cursor the client can resume from.
        let (status, body) = harness.client().call("db.status", db_params(account), None);
        assert_eq!(status, 200);
        assert_eq!(body["result"]["cursor"]["table"], "sync_outbox");
        assert_eq!(body["result"]["cursor"]["column"], "id");
        assert_eq!(body["result"]["cursor"]["value"], 2);
        assert_eq!(body["result"]["applied"], 2);
        harness.stop();
    }

    #[test]
    fn a_reader_token_cannot_start_the_ingest() {
        // § 15 п. 11: the client credential is a reader. `db.ingest` needs
        // `db:ingest`, which `pair` never grants.
        let account = account();
        let harness = Harness::start_with_db(
            account,
            vec![crate::db::tests_row(1, "message:1:1", "text", b"hello")],
        );
        let (status, body) =
            harness
                .client()
                .call("db.ingest", db_params(account), Some("idem-reader"));
        assert_eq!(status, 403, "{body}");
        assert_eq!(body["error"]["code"], 1003);
        assert!(
            body["error"]["message"]
                .as_str()
                .unwrap_or_default()
                .contains("db:ingest"),
            "the refusal must name the scope: {body}"
        );
        // Nothing was published.
        let (_, pull) = harness.client().call(
            "sync.pull",
            json!({"tenant": TENANT, "account": account.to_hex(), "since_seq": 0}),
            None,
        );
        assert_eq!(pull["result"]["ops"].as_array().map(Vec::len), Some(0));
        harness.stop();
    }

    #[test]
    fn an_ingest_token_can_start_the_ingest() {
        let account = account();
        let harness = Harness::start_with_db(
            account,
            vec![crate::db::tests_row(1, "message:1:1", "text", b"hello")],
        );
        let token = harness.enroll_ingest_device();
        let (status, body) = harness.client_with(&token).call(
            "db.ingest",
            db_params(account),
            Some("idem-ingest-role"),
        );
        assert_eq!(status, 200, "{body}");
        assert_eq!(body["result"]["pass"]["applied"], 1);
        harness.stop();
    }

    #[test]
    fn the_ingest_scheduler_is_off_by_default_and_survives_a_failed_pass() {
        // `interval_secs == 0` means no thread, but the pass stays callable. And a
        // failing pass must not take the process down — `run_db_ingest_once` swallows
        // it into a log line and leaves the state healthy.
        let account = account();
        let harness = Harness::start_with_db(account, Vec::new());
        let state = Arc::clone(&harness.state);
        assert!(
            spawn_db_ingest(Arc::clone(&state)).is_none(),
            "interval 0 must not start a thread"
        );
        // A source that always fails: two manual passes, neither panics.
        let failing = crate::db::DbIngest::with_connector(
            crate::db::DbIngestConfig::new(harness.data_dir(), TENANT, account),
            crate::logging::Logger::new(
                crate::logging::Level::Error,
                crate::logging::Destination::Stderr,
            ),
            Box::new(|_facts, _conn, _spec| {
                Ok(Box::new(crate::db::MockSource::failing(
                    crate::db::class::QUERY,
                )))
            }),
        )
        .expect("the config is valid");
        failing.unlock(harness.dek(), 1).expect("the key derives");
        let mut state = ServerState::new(state.config.clone()).expect("state builds");
        state.db = Some(Arc::new(failing));
        let state = Arc::new(state);
        run_db_ingest_once(&state);
        run_db_ingest_once(&state);
        assert!(
            state.db.as_ref().map(|db| db.health().fails) == Some(2),
            "each failed pass must be counted"
        );
        assert!(state.is_running());
        harness.stop();
    }

    #[test]
    fn retention_runs_and_logs_without_stopping_the_process() {
        let directory = TestDir::new("api-retention");
        let data_dir = directory.path().to_string_lossy().to_string();
        // A deterministic policy: a row cap of 0, so the pass must purge exactly the
        // one row written below regardless of the wall clock.
        let config = crate::config::AgentConfig {
            data_dir,
            listen: ListenMode::Loopback { port: 0 },
            retention_interval_secs: 0,
            retention: crate::store::RetentionPolicy {
                max_age_ms: None,
                max_ops_per_account: Some(0),
                max_bytes: None,
            },
            ..Default::default()
        };
        let state = Arc::new(ServerState::new(config).unwrap());
        let event = crate::store::Event::new(
            account(),
            "dev-a",
            crate::store::EventKind::ProfileUpdate,
            "profile:900",
            Some("username"),
            b"@old".to_vec(),
            crate::sync::clock::Hlc::new(1_000, 0),
        );
        state
            .sync
            .push(TENANT, &account(), &[event], 1_000)
            .unwrap();
        assert_eq!(state.sync.stamp(TENANT, &account()).unwrap().op_count, 1);
        run_retention_once(&state);
        // The scheduler purged, logged, and the process is still serving. This is the
        // `serve` requirement: retention must never take the endpoint down.
        assert_eq!(state.sync.stamp(TENANT, &account()).unwrap().op_count, 0);
        assert!(state.is_running());
        // A second pass over an empty store is a logged no-op, not an error.
        run_retention_once(&state);
    }

    #[test]
    fn idempotent_replay_returns_the_original_body() {
        let mut store = IdempotencyStore::new(4, 1_000);
        store.put("k1", "{\"applied\":1}", 100);
        assert_eq!(store.get("k1", 200).unwrap().body, "{\"applied\":1}");
        assert!(store.get("k1", 2_000).is_none(), "expired");
        assert!(store.get("other", 100).is_none());
    }

    #[test]
    fn store_is_bounded() {
        let mut store = IdempotencyStore::new(2, 10_000);
        store.put("a", "1", 1);
        store.put("b", "2", 2);
        store.put("c", "3", 3);
        assert_eq!(store.len(), 2);
        assert!(store.get("a", 3).is_none(), "oldest evicted");
        assert!(store.get("c", 3).is_some());
    }

    #[test]
    fn expired_entries_are_evicted_on_insert() {
        let mut store = IdempotencyStore::new(16, 100);
        store.put("a", "1", 0);
        store.put("b", "2", 1_000);
        assert!(store.get("a", 1_000).is_none());
        assert_eq!(store.len(), 1);
    }
}
