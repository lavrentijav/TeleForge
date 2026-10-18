//! Process entry point.
//!
//! Exit codes are part of the contract with the client's installer (see
//! [`teleforge_agent::error::exit`]), and `--self-test` is what the installer runs
//! *before* it commits a new version, so its output has to be machine-readable.

use teleforge_agent::api::server::{self, ServerState};
use teleforge_agent::cli::{self, Command, Options};
use teleforge_agent::config;
use teleforge_agent::error::{exit, Error, Result};
use teleforge_agent::logging::{Destination, Level, Logger};
use teleforge_agent::supervisor::Supervisor;

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let code = match run(args) {
        Ok(()) => exit::OK,
        Err(error) => {
            eprintln!("teleforge-agent: {error}");
            error.exit_code()
        }
    };
    std::process::exit(code);
}

fn run(args: Vec<String>) -> Result<()> {
    let options = cli::parse(args)?;
    let level = Level::parse(&options.log_level).unwrap_or(Level::Info);
    let logger = Logger::new(level, Destination::Stderr);

    match options.command.clone() {
        Command::Help => {
            print!("{}", cli::usage());
            Ok(())
        }
        Command::Version => {
            println!("teleforge-agent {}", teleforge_agent::AGENT_VERSION);
            Ok(())
        }
        Command::Serve => serve(&options, &logger),
        Command::SelfTest { tenant } => self_test(&options, tenant.as_deref()),
        Command::Unlock { tenant } => unlock(&options, tenant.as_deref(), &logger),
        Command::Lock => {
            logger.info("vault", "lock requested");
            // The key lives in the `serve` process. A separate CLI invocation can
            // only ask the running daemon, which is why this must go through the
            // socket rather than doing anything locally.
            Err(Error::NotImplemented(
                "`lock` over the control socket (SIGUSR1 is the fallback)",
            ))
        }
        Command::Pair {
            label,
            tenant,
            account,
        } => pair(&options, &label, tenant.as_deref(), account.as_deref()),
        Command::Revoke { device_id } => {
            logger.info("auth", &format!("revoke requested for {device_id}"));
            Err(Error::NotImplemented("device revocation"))
        }
        Command::Status => {
            let config = load_config(&options)?;
            let mut supervisor = Supervisor::new(now_ms());
            supervisor
                .slot(teleforge_agent::supervisor::Component::Api)
                .mark_running();
            println!(
                "{{\"agent_version\":\"{}\",\"protocol\":{},\"components\":{}}}",
                teleforge_agent::AGENT_VERSION,
                teleforge_agent::PROTOCOL_VERSION,
                supervisor.health_json()
            );
            let _ = config;
            Ok(())
        }
        Command::UpdateCheck => {
            logger.info("updater", "checking the signed manifest");
            Err(Error::NotImplemented(
                "manifest fetch and verification (needs an HTTP client)",
            ))
        }
    }
}

fn load_config(options: &Options) -> Result<config::AgentConfig> {
    let mut config = match &options.config_path {
        Some(path) => config::load(path)?,
        None => config::AgentConfig::default(),
    };
    if let Some(dir) = &options.data_dir {
        config.data_dir = dir.clone();
    }
    if let Some(socket) = &options.socket {
        config.socket_path = socket.clone();
    }
    if let Some(listen) = &options.listen {
        config.listen = parse_listen(listen)?;
    }
    if options.allow_plaintext {
        config.allow_plaintext_loopback = true;
    }
    if let Some(secs) = options.retention_interval_secs {
        config.retention_interval_secs = secs;
    }
    if let Some(days) = options.retention_max_age_days {
        let millis = i64::try_from(days)
            .ok()
            .and_then(|days| days.checked_mul(24 * 60 * 60 * 1_000))
            .ok_or_else(|| Error::Invalid("--retention-max-age is out of range".to_string()))?;
        config.retention.max_age_ms = Some(millis);
    }
    if let Some(connections) = options.max_connections {
        config.max_connections = connections;
    }
    if let Some(bytes) = options.max_body_bytes {
        config.max_body_bytes = bytes;
    }
    if let Some(path) = &options.tls_cert {
        config.tls.cert_path = Some(path.clone());
    }
    if let Some(path) = &options.tls_key {
        config.tls.key_path = Some(path.clone());
    }
    if let Some(secs) = options.ingest_interval_secs {
        config.db_ingest.interval_secs = secs;
    }
    if let Some(tenant) = &options.ingest_tenant {
        config.db_ingest.tenant = Some(tenant.to_ascii_lowercase());
    }
    if let Some(account) = &options.ingest_account {
        config.db_ingest.account = Some(account.to_ascii_lowercase());
    }
    if let Some(table) = &options.ingest_table {
        config.db_ingest.table = table.clone();
    }
    config.log_level = options.log_level.clone();
    Ok(config)
}

/// `pair` — mint a device token and print it **once**.
///
/// The token is written to stdout and nowhere else: the record on disk holds
/// `sha256(token)` (see `auth.rs`), so this is the only moment the value exists in
/// readable form. It is deliberately not passed through [`Logger`], which would
/// redact it and prove nothing.
fn pair(options: &Options, label: &str, tenant: Option<&str>, account: Option<&str>) -> Result<()> {
    use teleforge_agent::auth::{AuthStore, Scope, TokenRole};

    let config = load_config(options)?;
    if !std::path::Path::new(&config.data_dir).exists() {
        return Err(Error::Invalid(format!(
            "data_dir {} does not exist; the installer creates it",
            config.data_dir
        )));
    }
    if let Some(tenant) = tenant {
        if tenant.len() != 32 || !tenant.bytes().all(|byte| byte.is_ascii_hexdigit()) {
            return Err(Error::Invalid(
                "--tenant must be 32 hex characters (16 bytes)".to_string(),
            ));
        }
    }
    let account_id = match account {
        Some(hex) => Some(teleforge_agent::store::AccountId::from_hex(hex)?),
        None => None,
    };
    // § 15 п. 11: `pair` mints a **reader** unless the operator asks for an internal
    // token, and only that internal token carries `db:ingest`.
    let role = match options.role.as_deref() {
        Some(text) => {
            TokenRole::parse(text).ok_or_else(|| Error::Invalid(format!("bad --role {text:?}")))?
        }
        None => TokenRole::Reader,
    };
    let mut scopes = vec![Scope::Status, Scope::SyncRead, Scope::SyncWrite];
    if role == TokenRole::Ingest {
        scopes.push(Scope::DbIngest);
    }
    let mut store = AuthStore::load(&config.data_dir)?;
    let (token, record) = store.enroll_with_role(
        label,
        role,
        scopes.clone(),
        tenant.map(|value| value.to_ascii_lowercase()),
        account_id,
        now_ms(),
    )?;
    let scopes: Vec<&str> = scopes.iter().map(|scope| scope.as_str()).collect();
    println!(
        "{}",
        serde_json::json!({
            "device_id": record.device_id_hex(),
            "device_token": teleforge_agent::crypto::sig::to_hex(token.expose()),
            "label": record.label,
            "role": role.as_str(),
            "tenant": record.tenant,
            "account": record.account_id.map(|account| account.to_hex()),
            "scopes": scopes,
            "clients_file": store.path().to_string_lossy(),
        })
    );
    eprintln!(
        "teleforge-agent: the device token above is shown once and stored only as sha256; \
         put it in the client keychain now"
    );
    Ok(())
}

fn parse_listen(value: &str) -> Result<config::ListenMode> {
    if value == "socket" {
        return Ok(config::ListenMode::SocketOnly);
    }
    let (host, port) = value
        .rsplit_once(':')
        .ok_or_else(|| Error::Invalid(format!("--listen needs host:port, got {value:?}")))?;
    let port: u16 = port
        .parse()
        .map_err(|_| Error::Invalid(format!("bad port in {value:?}")))?;
    if host == "127.0.0.1" || host == "localhost" {
        return Ok(config::ListenMode::Loopback { port });
    }
    Ok(config::ListenMode::Network {
        addr: host.to_string(),
        port,
    })
}

fn serve(options: &Options, logger: &Logger) -> Result<()> {
    let config = load_config(options)?;
    // Refuse to start on a data dir that group/other can read: continuing would
    // put the salt next to ciphertext for anyone on the box.
    teleforge_agent::fsperm::assert_private(&config.data_dir)?;
    let state = ServerState::new(config)?;
    logger.info(
        "api",
        &format!(
            "starting agent {} protocol {}",
            teleforge_agent::AGENT_VERSION,
            teleforge_agent::PROTOCOL_VERSION
        ),
    );
    server::serve(state)
}

/// `--self-test` is the installer's gate. The report and the exit code live in
/// [`teleforge_agent::selftest`] so they are unit-tested; `main` only prints and
/// maps the result to a process exit code.
fn self_test(options: &Options, tenant: Option<&str>) -> Result<()> {
    let config = load_config(options)?;
    let report = teleforge_agent::selftest::run(&config.data_dir, tenant);
    println!("{}", report.to_json());
    if report.ok() {
        Ok(())
    } else {
        Err(Error::Unsupported(
            "self-test failed; see the checks array on stdout",
        ))
    }
}

fn unlock(options: &Options, tenant: Option<&str>, logger: &Logger) -> Result<()> {
    let config = load_config(options)?;
    let tenant_hex = tenant.ok_or_else(|| {
        Error::Invalid("`unlock` needs --tenant <hex>; see `status` for the ids".to_string())
    })?;
    let passphrase = teleforge_agent::secret::read_passphrase(options.passphrase_fd)?;
    let meta = teleforge_agent::vault::load_tenant_meta(&config.data_dir, tenant_hex)?;
    let unlocked = teleforge_agent::vault::unlock(&meta, &passphrase, now_ms())?;
    logger.info(
        "vault",
        &format!(
            "tenant {} unlocked (key_id {})",
            teleforge_agent::crypto::sig::to_hex(&unlocked.tenant_id),
            unlocked.key_id
        ),
    );
    Err(Error::NotImplemented(
        "handing the unlocked key to the running daemon over the control socket",
    ))
}

fn now_ms() -> u64 {
    use teleforge_agent::sync::clock::{Clock, SystemClock};
    SystemClock.now_ms()
}
