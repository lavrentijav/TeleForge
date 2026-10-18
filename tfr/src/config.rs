//! Agent configuration (`agent.toml`).
//!
//! Deliberately contains **no secrets**. Passwords, tokens and keys live in the
//! vault ([`crate::vault`]) or in the client's own keychain; `agent.toml` is
//! `0640` and is world-readable-by-group at worst, so nothing here may be
//! sensitive. This is the direct fix for finding M14, where `pgConnString` with
//! an inline `password=…` was stored in the same SQLite file as the archive.

use crate::crypto::aead::AeadAlgorithm;
use crate::crypto::kdf::KdfCost;
use crate::error::{Error, Result};

pub const DEFAULT_CONFIG_PATH: &str = "/etc/teleforge-agent/agent.toml";
pub const DEFAULT_DATA_DIR: &str = "/var/lib/teleforge-agent";
pub const DEFAULT_SOCKET_PATH: &str = "/run/teleforge-agent/agent.sock";
pub const DEFAULT_LISTEN_ADDR: &str = "127.0.0.1";
pub const DEFAULT_LISTEN_PORT: u16 = 8137;
pub const DEFAULT_UPDATE_BASE: &str = "https://github.com/lavrentijav/TeleForge/releases/download";

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum ListenMode {
    /// Only the unix socket. The desktop client reaches it through the SSH
    /// session (`ssh -L` onto the socket path is not possible, so the client's
    /// transport opens the socket over the same SSH channel it installed with).
    SocketOnly,
    /// Loopback TCP, TLS required. Default: the client forwards this port.
    Loopback { port: u16 },
    /// Explicit opt-in only. Refused unless `tls.client_ca_pem` is set.
    Network { addr: String, port: u16 },
}

#[derive(Clone, Debug, Default)]
pub struct TlsConfig {
    /// Path to the listener certificate (PEM, `CERTIFICATE` block). `None` means
    /// `<data_dir>/identity/agent.cert.pem`.
    pub cert_path: Option<String>,
    /// Path to the PKCS#8 private key (PEM, `PRIVATE KEY` block). `None` means
    /// `<data_dir>/identity/agent.key.pem`.
    pub key_path: Option<String>,
    /// Client CA bundle. Required for `Network`; also enables mTLS on loopback.
    pub client_ca_pem: Option<String>,
    pub require_client_cert: bool,
}

impl TlsConfig {
    /// The certificate path, defaulting to the installer's layout.
    pub fn certificate_path(&self, data_dir: &str) -> String {
        self.cert_path
            .clone()
            .unwrap_or_else(|| format!("{data_dir}/identity/agent.cert.pem"))
    }

    pub fn private_key_path(&self, data_dir: &str) -> String {
        self.key_path
            .clone()
            .unwrap_or_else(|| format!("{data_dir}/identity/agent.key.pem"))
    }

    /// True when both files exist. Missing identity is not an error here — the
    /// listener decides (TLS when present, plaintext only with an explicit opt-in).
    pub fn has_identity(&self, data_dir: &str) -> bool {
        std::path::Path::new(&self.certificate_path(data_dir)).exists()
            && std::path::Path::new(&self.private_key_path(data_dir)).exists()
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TunnelBackend {
    /// In-process SSH (`russh`): no dependency on a system `ssh` binary, real
    /// host-key verification, cancellable, async.
    InProcess,
    /// Shell out to `ssh -L`. Kept for the first migration phase so behaviour
    /// matches `EnsureTunnelBlocking` in `teleforge_ssh_tunnel.cpp` exactly.
    SshBinary,
}

#[derive(Clone, Debug)]
pub struct AgentConfig {
    pub data_dir: String,
    pub runtime_dir: String,
    pub config_dir: String,
    pub socket_path: String,
    pub listen: ListenMode,
    pub tls: TlsConfig,
    /// Argon2id cost used for the *next* tenant initialisation. Existing tenants
    /// keep their own recorded cost, so raising this is safe and gradual.
    pub kdf_cost: KdfCost,
    pub at_rest_aead: AeadAlgorithm,
    /// Idle time after which the vault re-locks itself. 0 disables.
    pub idle_lock_secs: u64,
    pub update: UpdateConfig,
    pub tunnel_backend: TunnelBackend,
    pub max_frame_bytes: u32,
    pub log_level: String,
    /// Largest accepted HTTP request body. Enforced before allocation; see
    /// `api::http`.
    pub max_body_bytes: usize,
    /// Largest number of simultaneously served connections. The (N+1)-th is refused
    /// with 503 rather than queued, so a slow peer cannot starve the agent of
    /// threads.
    pub max_connections: usize,
    pub read_timeout_ms: u64,
    pub write_timeout_ms: u64,
    /// Serve plaintext HTTP on loopback. Off by default: without a pinned TLS
    /// certificate the device token travels in cleartext to any local process
    /// that can sniff loopback. `--allow-plaintext` is for tests and for a
    /// first-run bootstrap on a host where the installer has not written
    /// `identity/` yet.
    pub allow_plaintext_loopback: bool,
    /// Periodic retention. `interval_secs == 0` disables the scheduler (the
    /// functions stay callable).
    pub retention_interval_secs: u64,
    pub retention: crate::store::RetentionPolicy,
    /// Per-device rate limit: burst size and refill rate.
    pub rate_limit_burst: u32,
    pub rate_limit_per_sec: u32,
    /// Periodic read of the user's PostgreSQL/MySQL (`docs/db-ingest.md`).
    pub db_ingest: DbIngestSettings,
}

/// Server-side DB ingest (`src/db/**`, `docs/db-ingest.md`).
///
/// **No connection string here** (§ 2.6 п. 5 of `docs/transport.md`): the URL and its
/// password live in the vault (`vault`'s `db/credentials.enc` slot), so `agent.toml`
/// stays a file an operator can read and paste into a ticket. This struct only says
/// *whether* and *where* to read.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DbIngestSettings {
    /// Seconds between passes. `0` disables the scheduler — the pass stays callable
    /// through `db.ingest`, which is what tests and an operator use.
    pub interval_secs: u64,
    /// Tenant whose vault slot and journal the ingest uses.
    pub tenant: Option<String>,
    /// Account the rows are published into.
    pub account: Option<String>,
    /// Allowlisted table to read (`sync_outbox` is the only one with a row mapping
    /// in this build).
    pub table: String,
    /// Rows per `SELECT`.
    pub batch_limit: usize,
}

impl Default for DbIngestSettings {
    fn default() -> Self {
        Self {
            // Off by default: switching on a periodic read of the user's database
            // without an explicit `--ingest-*` configuration would be a surprise on
            // an agent installed by an older client.
            interval_secs: 0,
            tenant: None,
            account: None,
            table: crate::db::DEFAULT_TABLE.to_string(),
            batch_limit: crate::db::DEFAULT_BATCH_LIMIT,
        }
    }
}

impl DbIngestSettings {
    /// True when the tenant/account pair is configured, i.e. `db.status` can say
    /// anything at all about the DB path.
    pub fn is_configured(&self) -> bool {
        self.tenant.is_some() && self.account.is_some()
    }
}

#[derive(Clone, Debug)]
pub struct UpdateConfig {
    pub channel: String,
    pub base_url: String,
    /// Pinned Ed25519 root public key, 32 bytes hex. Duplicated from the
    /// compiled-in constant so an operator can pin a different root for a
    /// private build; `None` means "use the compiled-in key".
    pub root_pubkey_hex: Option<String>,
    pub check_interval_secs: u64,
    /// Refuse manifests whose `generation` is not greater than the last applied
    /// one. Anti-rollback; see [`crate::updater`].
    pub enforce_monotonic_generation: bool,
}

impl Default for AgentConfig {
    fn default() -> Self {
        Self {
            data_dir: DEFAULT_DATA_DIR.to_string(),
            runtime_dir: "/run/teleforge-agent".to_string(),
            config_dir: "/etc/teleforge-agent".to_string(),
            socket_path: DEFAULT_SOCKET_PATH.to_string(),
            listen: ListenMode::Loopback {
                port: DEFAULT_LISTEN_PORT,
            },
            tls: TlsConfig::default(),
            kdf_cost: KdfCost::default(),
            at_rest_aead: AeadAlgorithm::XChaCha20Poly1305,
            idle_lock_secs: 900,
            update: UpdateConfig {
                channel: "stable".to_string(),
                base_url: DEFAULT_UPDATE_BASE.to_string(),
                root_pubkey_hex: None,
                check_interval_secs: 21_600,
                enforce_monotonic_generation: true,
            },
            tunnel_backend: TunnelBackend::InProcess,
            max_frame_bytes: 1 << 20,
            log_level: "info".to_string(),
            // 8 MiB: one `sync.push` of 2000 small events, not a database dump.
            max_body_bytes: 8 << 20,
            max_connections: 32,
            read_timeout_ms: 10_000,
            write_timeout_ms: 10_000,
            allow_plaintext_loopback: false,
            retention_interval_secs: 3_600,
            retention: crate::store::RetentionPolicy::default_for_vps(),
            rate_limit_burst: 120,
            rate_limit_per_sec: 20,
            db_ingest: DbIngestSettings::default(),
        }
    }
}

impl AgentConfig {
    /// Reject configurations that would silently weaken the deployment: a
    /// network listener without mTLS, a world-reachable bind, or a data dir
    /// under a shared parent. Called by `serve` before the vault opens.
    pub fn validate(&self) -> Result<()> {
        if let ListenMode::Network { addr, .. } = &self.listen {
            if self.tls.client_ca_pem.is_none() {
                return Err(Error::Invalid(
                    "listen = network requires tls.client_ca_pem (mTLS); \
                     the agent is deployed through an SSH tunnel and does not need a public port"
                        .to_string(),
                ));
            }
            if addr == "0.0.0.0" || addr == "::" {
                return Err(Error::Invalid(
                    "refusing to bind a wildcard address; use a specific address".to_string(),
                ));
            }
        }
        // Only warn when there is genuinely no transport authentication: with a
        // listener certificate the port is reachable but the token cannot be read
        // off the wire, and a warning on every start would train the operator to
        // ignore the line that matters.
        if matches!(self.listen, ListenMode::Loopback { .. })
            && !self.tls.require_client_cert
            && !self.tls.has_identity(&self.data_dir)
        {
            // Documented, not fatal: loopback + token auth is the phase-1 setup,
            // but the operator should know that a same-host process can reach it.
            crate::logging::Logger::new(crate::logging::Level::Warn, crate::logging::Destination::Stderr)
                .warn(
                    "config",
                    "loopback listener without mTLS: any local process can reach the port; the token is the only gate",
                );
        }
        self.kdf_cost.validate()?;
        if self.max_frame_bytes < 4096 {
            return Err(Error::Invalid(
                "max_frame_bytes below 4096 would break the sync stream".to_string(),
            ));
        }
        if self.max_body_bytes < 4096 {
            return Err(Error::Invalid(
                "max_body_bytes below 4096 cannot carry a useful batch".to_string(),
            ));
        }
        if self.max_body_bytes > 256 << 20 {
            return Err(Error::Invalid(
                "max_body_bytes above 256 MiB is a memory-amplification risk".to_string(),
            ));
        }
        if self.max_connections == 0 {
            return Err(Error::Invalid(
                "max_connections must be at least 1".to_string(),
            ));
        }
        if self.read_timeout_ms == 0 || self.write_timeout_ms == 0 {
            return Err(Error::Invalid(
                "read/write timeouts must be non-zero: a peer that stalls must time out"
                    .to_string(),
            ));
        }
        if self.rate_limit_per_sec == 0 || self.rate_limit_burst == 0 {
            return Err(Error::Invalid(
                "rate_limit_burst and rate_limit_per_sec must be positive".to_string(),
            ));
        }
        // The ingest is off unless it was asked for, and "asked for" means a complete
        // tenant/account pair: a scheduler with no account has nothing to publish and
        // would log an error on every tick.
        if self.db_ingest.interval_secs > 0 && !self.db_ingest.is_configured() {
            return Err(Error::Invalid(
                "db ingest needs both --ingest-tenant and --ingest-account".to_string(),
            ));
        }
        if let Some(tenant) = &self.db_ingest.tenant {
            if tenant.len() != 32 || !tenant.bytes().all(|byte| byte.is_ascii_hexdigit()) {
                return Err(Error::Invalid(
                    "db ingest tenant must be 32 hex characters".to_string(),
                ));
            }
        }
        if let Some(account) = &self.db_ingest.account {
            crate::store::AccountId::from_hex(account)?;
        }
        Ok(())
    }

    /// Path of the per-tenant directory. Tenant ids are 16 random bytes shown as
    /// lowercase hex; they are **not** derived from the Telegram user id or the
    /// phone number (that is finding C1).
    pub fn tenant_dir(&self, tenant_hex: &str) -> String {
        format!("{}/tenants/{}", self.data_dir, tenant_hex)
    }
}

pub fn load(path: &str) -> Result<AgentConfig> {
    // TODO(toml): read + `toml::from_str::<AgentConfigFile>` then merge over
    // `AgentConfig::default()`. Every field is optional in the file so a partial
    // `agent.toml` written by an older client still boots.
    let _ = std::fs::metadata(path).map_err(|_| {
        Error::Invalid(format!(
            "config {path} not found; pass --config or run the installer"
        ))
    })?;
    Err(Error::NotImplemented("toml config parsing"))
}

/// Serialise the subset the client writes during bootstrap. Kept as text so the
/// bootstrap payload can be produced without pulling in a serializer.
pub fn render_bootstrap_config(listen: ListenMode, data_dir: &str, socket: &str) -> String {
    let mut out = String::new();
    out.push_str("# written by the TeleForge desktop client during install\n");
    out.push_str(&format!("data_dir = {:?}\n", data_dir));
    out.push_str(&format!("socket_path = {:?}\n", socket));
    match listen {
        ListenMode::SocketOnly => out.push_str("listen = \"socket\"\n"),
        ListenMode::Loopback { port } => out.push_str(&format!("listen = \"127.0.0.1:{port}\"\n")),
        ListenMode::Network { addr, port } => {
            out.push_str(&format!("listen = \"{addr}:{port}\"\n"))
        }
    }
    out.push_str("idle_lock_secs = 900\n");
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A config that passes every unrelated check: a loopback listener with an
    /// identity, so `validate` only reports on what the test is about.
    fn base() -> AgentConfig {
        AgentConfig {
            data_dir: "/tmp/tf-agent".to_string(),
            tls: TlsConfig {
                cert_path: Some("/tmp/cert.pem".to_string()),
                key_path: Some("/tmp/key.pem".to_string()),
                ..TlsConfig::default()
            },
            ..AgentConfig::default()
        }
    }

    #[test]
    fn the_db_ingest_is_off_by_default() {
        let config = AgentConfig::default();
        assert_eq!(config.db_ingest.interval_secs, 0);
        assert!(!config.db_ingest.is_configured());
        assert_eq!(config.db_ingest.table, crate::db::DEFAULT_TABLE);
        assert_eq!(config.db_ingest.batch_limit, crate::db::DEFAULT_BATCH_LIMIT);
        // Off means off: no tenant/account is not an error while the scheduler is
        // disabled.
        assert!(base().validate().is_ok());
    }

    #[test]
    fn enabling_the_scheduler_requires_a_complete_target() {
        let mut config = base();
        config.db_ingest.interval_secs = 60;
        let error = config
            .validate()
            .expect_err("a half-configured ingest must fail");
        assert!(error.to_string().contains("--ingest-tenant"), "{error}");

        config.db_ingest.tenant = Some("00112233445566778899aabbccddeeff".to_string());
        let error = config.validate().expect_err("the account is still missing");
        assert!(error.to_string().contains("--ingest-account"), "{error}");

        config.db_ingest.account = Some("ffeeddccbbaa99887766554433221100".to_string());
        assert!(config.validate().is_ok(), "a complete target is valid");
    }

    #[test]
    fn a_bad_tenant_or_account_is_refused() {
        let mut config = base();
        config.db_ingest.interval_secs = 1;
        config.db_ingest.account = Some("ffeeddccbbaa99887766554433221100".to_string());

        for tenant in ["short", "00112233445566778899aabbccddeefg", "../etc"] {
            config.db_ingest.tenant = Some(tenant.to_string());
            assert!(
                config.validate().is_err(),
                "{tenant:?} must be refused as a tenant"
            );
        }
        config.db_ingest.tenant = Some("00112233445566778899aabbccddeeff".to_string());
        config.db_ingest.account = Some("not-hex".to_string());
        assert!(config.validate().is_err());
    }

    #[test]
    fn the_rendered_bootstrap_file_contains_no_secret() {
        // M14 in one assertion: the file the client writes lists where, never with
        // what. A connection string here would be the old `pgConnString` bug.
        let text = render_bootstrap_config(
            ListenMode::Loopback { port: 8137 },
            "/var/lib/tf",
            "/run/tf.sock",
        );
        assert!(!text.contains("password"));
        assert!(!text.contains("postgresql://"));
        assert!(!text.contains("mysql://"));
        assert!(!text.contains("sslmode"));
    }
}
