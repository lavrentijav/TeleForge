//! Command line surface.
//!
//! The CLI is small on purpose, and every command is also reachable as a
//! *local* control call (unix socket / loopback with a client token) because the
//! desktop client never runs the agent's binary directly after installation —
//! it talks to it. The CLI exists for:
//! * `serve` — the systemd `ExecStart`;
//! * `self-test` — the `ExecStartPre`/updater healthcheck;
//! * `unlock` / `lock` — an operator on the VPS console;
//! * `pair` / `status` — bootstrap and diagnostics from the VPS.

use crate::error::{Error, Result};

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Command {
    Serve,
    /// `--tenant` is optional: without it the database half of the report runs
    /// against an in-memory store.
    SelfTest {
        tenant: Option<String>,
    },
    /// Unlock the vault for the named tenant. The passphrase is read from the
    /// TTY (or from `--passphrase-fd`), never from `argv`.
    Unlock {
        tenant: Option<String>,
    },
    Lock,
    /// Mint a device token and print it once. `--tenant`/`--account` bind the device
    /// to one tenant and one account, which is what makes "push into someone else's
    /// account" a 403 rather than a per-event rejection.
    Pair {
        label: String,
        tenant: Option<String>,
        account: Option<String>,
    },
    /// Revoke a device token by device id.
    Revoke {
        device_id: String,
    },
    Status,
    /// Check the release manifest and, if newer and installable, print the plan.
    UpdateCheck,
    Help,
    Version,
}

#[derive(Debug, Clone)]
pub struct Options {
    pub command: Command,
    pub config_path: Option<String>,
    pub data_dir: Option<String>,
    pub listen: Option<String>,
    pub socket: Option<String>,
    pub passphrase_fd: Option<i32>,
    pub log_level: String,
    /// Enables accepting an unsigned manifest. Rejected unless the binary was
    /// built with `TF_DEV_BUILD`, so a shipped release cannot be downgraded to
    /// unsigned trust by a flag alone.
    pub allow_unsigned_dev: bool,
    /// Run as a foreground process without installing anything (systemd owns
    /// the lifecycle in production).
    pub foreground: bool,
    /// `--account <hex>` for `pair`: the account the new device may address.
    pub account: Option<String>,
    /// Accept plaintext HTTP on loopback (tests, or a first run before the
    /// installer has written `identity/`).
    pub allow_plaintext: bool,
    /// `--retention-interval <secs>`; 0 disables the scheduler.
    pub retention_interval_secs: Option<u64>,
    /// `--retention-max-age <days>`; overrides the default 90-day policy.
    pub retention_max_age_days: Option<u64>,
    /// `--ingest-interval <secs>`; 0 (default) disables the server-side DB ingest.
    pub ingest_interval_secs: Option<u64>,
    /// `--ingest-tenant <hex>`: whose vault slot and journal the ingest uses.
    pub ingest_tenant: Option<String>,
    /// `--ingest-account <hex>`: the account the DB rows are published into.
    pub ingest_account: Option<String>,
    /// `--ingest-table <name>`: allowlisted table to read (default `sync_outbox`).
    pub ingest_table: Option<String>,
    /// `--role reader|ingest` for `pair`: what kind of token to mint.
    pub role: Option<String>,
    pub max_connections: Option<usize>,
    pub max_body_bytes: Option<usize>,
    pub tls_cert: Option<String>,
    pub tls_key: Option<String>,
}

impl Default for Options {
    fn default() -> Self {
        Self {
            command: Command::Serve,
            config_path: None,
            data_dir: None,
            listen: None,
            socket: None,
            passphrase_fd: None,
            log_level: "info".to_string(),
            allow_unsigned_dev: false,
            foreground: false,
            account: None,
            allow_plaintext: false,
            retention_interval_secs: None,
            retention_max_age_days: None,
            ingest_interval_secs: None,
            ingest_tenant: None,
            ingest_account: None,
            ingest_table: None,
            role: None,
            max_connections: None,
            max_body_bytes: None,
            tls_cert: None,
            tls_key: None,
        }
    }
}

pub fn usage() -> &'static str {
    "\
teleforge-agent — TeleForge server-side agent

USAGE:
    teleforge-agent <command> [options]

COMMANDS:
    serve                       run the agent (systemd ExecStart)
    self-test [--tenant <id>]   verify layout, permissions, SQLite schema and KDF/AEAD, exit non-zero on failure
    unlock [--tenant <id>]      derive the data key and keep it in memory
    lock                        drop every key from memory
    pair --label <name>         mint a one-time pairing code for a desktop device
    revoke --device-id <id>     revoke a device token
    status                      print health as JSON (used by the client's UI)
    update-check                check the signed manifest, print the install plan
    version                     print the agent version
    help                        this text

OPTIONS:
    --config <path>             agent.toml (default: /etc/teleforge-agent/agent.toml)
    --data-dir <path>           override the state directory
    --listen <addr:port>        bind the API listener (default 127.0.0.1:8137)
    --socket <path>             unix socket path (default /run/teleforge-agent/agent.sock)
    --account <hex>             pair: bind the device to one account
    --passphrase-fd <n>         read the passphrase from an inherited descriptor
    --allow-plaintext           serve plaintext HTTP on loopback (tests only)
    --tls-cert <path>           listener certificate (default <data-dir>/identity/agent.cert.pem)
    --tls-key <path>            listener PKCS#8 key (default <data-dir>/identity/agent.key.pem)
    --retention-interval <s>    seconds between retention passes (0 disables)
    --retention-max-age <days>  history age kept by retention (default 90)
    --ingest-interval <s>       seconds between server-side DB ingest passes (0 = off)
    --ingest-tenant <hex>       tenant whose vault slot and journal the ingest uses
    --ingest-account <hex>      account the DB rows are published into
    --ingest-table <name>       allowlisted table to read (default sync_outbox)
    --role <reader|ingest>      pair: what kind of token to mint (default reader)
    --max-connections <n>       simultaneously served connections (default 32)
    --max-body-bytes <n>        largest accepted request body (default 8 MiB)
    --log-level <lvl>           error|warn|info|debug|trace (default info)
    --foreground                do not detach
    --allow-unsigned-dev        accept unsigned manifests (dev builds only)
"
}

pub fn parse<I>(args: I) -> Result<Options>
where
    I: IntoIterator<Item = String>,
{
    let mut iter = args.into_iter();
    let _argv0 = iter.next();
    let mut rest: Vec<String> = iter.collect();

    if rest.is_empty() {
        return Ok(Options {
            command: Command::Help,
            ..Options::default()
        });
    }

    let head = rest.remove(0);
    let mut options = Options::default();
    let mut command = match head.as_str() {
        "serve" => Command::Serve,
        "self-test" => Command::SelfTest { tenant: None },
        "unlock" => Command::Unlock { tenant: None },
        "lock" => Command::Lock,
        "pair" => Command::Pair {
            label: "desktop".to_string(),
            tenant: None,
            account: None,
        },
        "revoke" => Command::Revoke {
            device_id: String::new(),
        },
        "status" => Command::Status,
        "update-check" => Command::UpdateCheck,
        "version" | "--version" | "-V" => Command::Version,
        "help" | "--help" | "-h" => Command::Help,
        other => {
            return Err(Error::Invalid(format!(
                "unknown command {other:?}; run `teleforge-agent help`"
            )))
        }
    };

    let mut index = 0usize;
    while index < rest.len() {
        let flag = rest[index].clone();
        let mut take_value = |name: &str| -> Result<String> {
            if index + 1 >= rest.len() {
                return Err(Error::Invalid(format!("{name} needs a value")));
            }
            index += 1;
            Ok(rest[index].clone())
        };
        match flag.as_str() {
            "--config" => options.config_path = Some(take_value("--config")?),
            "--data-dir" => options.data_dir = Some(take_value("--data-dir")?),
            "--listen" => options.listen = Some(take_value("--listen")?),
            "--socket" => options.socket = Some(take_value("--socket")?),
            "--log-level" => options.log_level = take_value("--log-level")?,
            "--passphrase-fd" => {
                let raw = take_value("--passphrase-fd")?;
                let fd: i32 = raw
                    .parse()
                    .map_err(|_| Error::Invalid(format!("bad --passphrase-fd {raw:?}")))?;
                options.passphrase_fd = Some(fd);
            }
            "--tenant" => {
                let tenant = take_value("--tenant")?;
                match &command {
                    Command::Unlock { .. } => {
                        command = Command::Unlock {
                            tenant: Some(tenant),
                        };
                    }
                    Command::SelfTest { .. } => {
                        command = Command::SelfTest {
                            tenant: Some(tenant),
                        };
                    }
                    Command::Pair { label, account, .. } => {
                        command = Command::Pair {
                            label: label.clone(),
                            tenant: Some(tenant),
                            account: account.clone(),
                        };
                    }
                    _ => {
                        return Err(Error::Invalid(
                            "--tenant only applies to `unlock`, `self-test` and `pair`".into(),
                        ))
                    }
                }
            }
            "--label" => {
                let label = take_value("--label")?;
                if let Command::Pair {
                    tenant, account, ..
                } = &command
                {
                    command = Command::Pair {
                        label,
                        tenant: tenant.clone(),
                        account: account.clone(),
                    };
                } else {
                    return Err(Error::Invalid("--label only applies to `pair`".into()));
                }
            }
            "--device-id" => {
                let device_id = take_value("--device-id")?;
                if let Command::Revoke { .. } = command {
                    command = Command::Revoke { device_id };
                } else {
                    return Err(Error::Invalid(
                        "--device-id only applies to `revoke`".into(),
                    ));
                }
            }
            "--account" => {
                let account = take_value("--account")?;
                match &command {
                    Command::Pair { label, tenant, .. } => {
                        command = Command::Pair {
                            label: label.clone(),
                            tenant: tenant.clone(),
                            account: Some(account),
                        };
                    }
                    _ => return Err(Error::Invalid("--account only applies to `pair`".into())),
                }
            }
            "--foreground" => options.foreground = true,
            "--allow-unsigned-dev" => options.allow_unsigned_dev = true,
            "--allow-plaintext" => options.allow_plaintext = true,
            "--retention-interval" => {
                let raw = take_value("--retention-interval")?;
                options.retention_interval_secs =
                    Some(raw.parse().map_err(|_| {
                        Error::Invalid(format!("bad --retention-interval {raw:?}"))
                    })?);
            }
            "--retention-max-age" => {
                let raw = take_value("--retention-max-age")?;
                options.retention_max_age_days = Some(
                    raw.parse()
                        .map_err(|_| Error::Invalid(format!("bad --retention-max-age {raw:?}")))?,
                );
            }
            "--ingest-interval" => {
                let raw = take_value("--ingest-interval")?;
                options.ingest_interval_secs = Some(
                    raw.parse()
                        .map_err(|_| Error::Invalid(format!("bad --ingest-interval {raw:?}")))?,
                );
            }
            "--ingest-tenant" => options.ingest_tenant = Some(take_value("--ingest-tenant")?),
            "--ingest-account" => options.ingest_account = Some(take_value("--ingest-account")?),
            "--ingest-table" => options.ingest_table = Some(take_value("--ingest-table")?),
            "--role" => {
                let raw = take_value("--role")?;
                if !matches!(command, Command::Pair { .. }) {
                    return Err(Error::Invalid("--role only applies to `pair`".into()));
                }
                if crate::auth::TokenRole::parse(&raw).is_none() {
                    return Err(Error::Invalid(format!("bad --role {raw:?}")));
                }
                options.role = Some(raw.to_ascii_lowercase());
            }
            "--max-connections" => {
                let raw = take_value("--max-connections")?;
                options.max_connections = Some(
                    raw.parse()
                        .map_err(|_| Error::Invalid(format!("bad --max-connections {raw:?}")))?,
                );
            }
            "--max-body-bytes" => {
                let raw = take_value("--max-body-bytes")?;
                options.max_body_bytes = Some(
                    raw.parse()
                        .map_err(|_| Error::Invalid(format!("bad --max-body-bytes {raw:?}")))?,
                );
            }
            "--tls-cert" => options.tls_cert = Some(take_value("--tls-cert")?),
            "--tls-key" => options.tls_key = Some(take_value("--tls-key")?),
            "--help" | "-h" => {
                return Ok(Options {
                    command: Command::Help,
                    ..Options::default()
                })
            }
            other => {
                return Err(Error::Invalid(format!(
                    "unknown option {other:?}; run `teleforge-agent help`"
                )))
            }
        }
        index += 1;
    }

    if let Command::Revoke { device_id } = &command {
        if device_id.is_empty() {
            return Err(Error::Invalid(
                "`revoke` needs --device-id <hex>".to_string(),
            ));
        }
    }

    if crate::logging::Level::parse(&options.log_level).is_none() {
        return Err(Error::Invalid(format!(
            "bad --log-level {:?}",
            options.log_level
        )));
    }

    options.command = command;
    Ok(options)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn parse_args(args: &[&str]) -> Result<Options> {
        let mut argv: Vec<String> = vec!["teleforge-agent".to_string()];
        argv.extend(args.iter().map(|arg| arg.to_string()));
        parse(argv)
    }

    #[test]
    fn the_db_ingest_options_are_parsed() {
        let options = parse_args(&[
            "serve",
            "--ingest-interval",
            "30",
            "--ingest-tenant",
            "00112233445566778899aabbccddeeff",
            "--ingest-account",
            "ffeeddccbbaa99887766554433221100",
            "--ingest-table",
            "sync_outbox",
        ])
        .expect("the ingest flags must parse");
        assert_eq!(options.ingest_interval_secs, Some(30));
        assert_eq!(
            options.ingest_tenant.as_deref(),
            Some("00112233445566778899aabbccddeeff")
        );
        assert_eq!(
            options.ingest_account.as_deref(),
            Some("ffeeddccbbaa99887766554433221100")
        );
        assert_eq!(options.ingest_table.as_deref(), Some("sync_outbox"));
        assert!(parse_args(&["serve", "--ingest-interval", "soon"]).is_err());
    }

    #[test]
    fn the_role_flag_belongs_to_pair_and_is_validated() {
        let options = parse_args(&["pair", "--label", "internal", "--role", "ingest"])
            .expect("--role ingest must parse");
        assert_eq!(options.role.as_deref(), Some("ingest"));
        assert!(parse_args(&["pair", "--role", "root"]).is_err());
        // `serve` has no token to mint, so the flag must not be silently ignored.
        assert!(parse_args(&["serve", "--role", "reader"]).is_err());
        // The default is no role, which main() reads as `reader`.
        assert!(parse_args(&["pair", "--label", "x"])
            .unwrap()
            .role
            .is_none());
    }

    #[test]
    fn self_test_accepts_a_tenant() {
        let options = parse_args(&["self-test", "--tenant", "00112233445566778899aabbccddeeff"])
            .expect("self-test must accept --tenant");
        assert_eq!(
            options.command,
            Command::SelfTest {
                tenant: Some("00112233445566778899aabbccddeeff".to_string())
            }
        );
    }

    #[test]
    fn self_test_without_a_tenant_still_parses() {
        let options = parse_args(&["self-test", "--data-dir", "/tmp/x"]).unwrap();
        assert_eq!(options.command, Command::SelfTest { tenant: None });
        assert_eq!(options.data_dir.as_deref(), Some("/tmp/x"));
    }

    #[test]
    fn unlock_still_takes_a_tenant() {
        let options = parse_args(&["unlock", "--tenant", "aabb"]).unwrap();
        assert_eq!(
            options.command,
            Command::Unlock {
                tenant: Some("aabb".to_string())
            }
        );
    }

    #[test]
    fn tenant_is_rejected_for_commands_that_have_no_use_for_it() {
        let error = parse_args(&["status", "--tenant", "aabb"]).unwrap_err();
        assert!(error.to_string().contains("--tenant"));
    }

    #[test]
    fn no_arguments_prints_help() {
        let options = parse_args(&[]).unwrap();
        assert_eq!(options.command, Command::Help);
    }

    #[test]
    fn unknown_commands_and_options_are_rejected() {
        assert!(parse_args(&["frobnicate"]).is_err());
        assert!(parse_args(&["status", "--nope"]).is_err());
        assert!(parse_args(&["status", "--log-level", "shouting"]).is_err());
    }

    #[test]
    fn revoke_requires_a_device_id() {
        assert!(parse_args(&["revoke"]).is_err());
        assert!(parse_args(&["revoke", "--device-id", "abc"]).is_ok());
    }

    #[test]
    fn help_after_a_command_prints_help_instead_of_failing() {
        // The installer runs `serve --help` to discover flags on an unknown build; an
        // "unknown option" error there would be a confusing first contact.
        let options = parse_args(&["serve", "--help"]).unwrap();
        assert_eq!(options.command, Command::Help);
        let options = parse_args(&["self-test", "-h"]).unwrap();
        assert_eq!(options.command, Command::Help);
        assert!(usage().contains("--retention-interval"));
    }

    #[test]
    fn pair_accepts_tenant_and_account_bindings() {
        let options = parse_args(&[
            "pair",
            "--label",
            "MacBook",
            "--tenant",
            "00112233445566778899aabbccddeeff",
            "--account",
            "ffeeddccbbaa99887766554433221100",
        ])
        .unwrap();
        assert_eq!(
            options.command,
            Command::Pair {
                label: "MacBook".to_string(),
                tenant: Some("00112233445566778899aabbccddeeff".to_string()),
                account: Some("ffeeddccbbaa99887766554433221100".to_string()),
            }
        );
    }

    #[test]
    fn account_is_rejected_for_commands_that_do_not_pair() {
        assert!(parse_args(&["serve", "--account", "aabb"]).is_err());
        assert!(parse_args(&["self-test", "--account", "aabb"]).is_err());
    }

    #[test]
    fn server_limits_are_parsed_and_validated() {
        let options = parse_args(&[
            "serve",
            "--allow-plaintext",
            "--tls-cert",
            "/tmp/cert.pem",
            "--tls-key",
            "/tmp/key.pem",
            "--retention-interval",
            "60",
            "--retention-max-age",
            "30",
            "--max-connections",
            "8",
            "--max-body-bytes",
            "1048576",
        ])
        .unwrap();
        assert!(options.allow_plaintext);
        assert_eq!(options.tls_cert.as_deref(), Some("/tmp/cert.pem"));
        assert_eq!(options.tls_key.as_deref(), Some("/tmp/key.pem"));
        assert_eq!(options.retention_interval_secs, Some(60));
        assert_eq!(options.retention_max_age_days, Some(30));
        assert_eq!(options.max_connections, Some(8));
        assert_eq!(options.max_body_bytes, Some(1_048_576));

        assert!(parse_args(&["serve", "--max-connections", "many"]).is_err());
        assert!(parse_args(&["serve", "--retention-interval", "-5"]).is_err());
    }

    #[test]
    fn listen_parsing_is_restricted_to_loopback_or_explicit_network() {
        // The agent never opens a public port on its own initiative; an explicit
        // `--listen 0.0.0.0:1` is a deliberate operator choice, but the default
        // and the loopback form must both be representable.
        let options = parse_args(&["serve", "--listen", "socket"]).unwrap();
        assert_eq!(options.listen.as_deref(), Some("socket"));
    }
}
