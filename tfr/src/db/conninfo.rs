//! Connection-string parsing, and the *only* redaction point of the DB path.
//!
//! # What is parsed, and why not `from_url`
//!
//! The drivers can parse their own URLs, but both of them reject or ignore
//! parameters they do not know (`ssl-mode` for `mysql`, `sslmode=verify-full` for
//! `tokio-postgres`), and the agent's own TLS policy (§ 2.6 of
//! `docs/transport.md`) needs `verify-full` to be the *default* rather than
//! something the driver happens to interpret. So the string is parsed once, here,
//! into [`ConnFacts`]: the non-secret facts that `db.status` is allowed to publish,
//! plus the TLS mode the agent enforces. The drivers are then configured
//! programmatically from those facts.
//!
//! # What must never be logged
//!
//! The password, and the URL as a whole (it contains the password). [`ConnFacts`]
//! deliberately has no field that can hold it, and its `Debug` is derived from
//! redacted values only; [`ConnFacts::redacted`] produces the one string that may
//! appear in a log line.
//!
//! # Accepted forms
//!
//! ```text
//! postgresql://user:pass@host:5432/db?sslmode=verify-full&sslrootcert=/etc/pg/ca.pem
//! postgres://user:pass@host/db
//! mysql://user:pass@host:3306/db?ssl-mode=verify-full&ssl-ca=/etc/mysql/ca.pem
//! mysql://user:pass@host/db?ssl-mode=require
//! ```
//!
//! A key/value DSN (`host=… user=… password=…`) is **not** accepted: it is the form
//! `pgConnString` used in the C++ tree (finding M14), and supporting it would make
//! "which part of this string is a secret" ambiguous again.

use crate::db::Engine;
use crate::error::{Error, Result};

/// TLS policy of a connection. Ordered from strongest to weakest so
/// [`SslMode::is_weaker_than_verify_full`] is a comparison, not a match.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub enum SslMode {
    /// Certificate chain *and* hostname checked. The default.
    VerifyFull,
    /// Certificate chain checked, hostname not. Accepted with a warning.
    VerifyCa,
    /// TLS, but no certificate validation. Accepted with a warning.
    Require,
    /// Try TLS, fall back to cleartext. Accepted with a warning.
    Prefer,
    /// Cleartext. Refused.
    Disable,
}

impl SslMode {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::VerifyFull => "verify-full",
            Self::VerifyCa => "verify-ca",
            Self::Require => "require",
            Self::Prefer => "prefer",
            Self::Disable => "disable",
        }
    }

    /// `libpq` and mysql spell the same idea differently; accept both, and accept
    /// every spelling of "no TLS" so it can be *refused* instead of being treated as
    /// an unknown value that silently defaults to the safe mode.
    pub fn parse(value: &str) -> Option<Self> {
        match value.trim().to_ascii_lowercase().as_str() {
            "verify-full" | "verify_full" | "verifyfull" => Some(Self::VerifyFull),
            "verify-ca" | "verify_ca" | "verifyca" => Some(Self::VerifyCa),
            "require" | "required" => Some(Self::Require),
            "prefer" | "preferred" => Some(Self::Prefer),
            "disable" | "disabled" | "off" | "false" | "none" | "trust" => Some(Self::Disable),
            _ => None,
        }
    }

    /// True when the mode is not `verify-full`. The ingest warns once per connection
    /// and records the mode in `db.status.tls`, so "we are on a weaker mode" is
    /// visible instead of folklore.
    pub fn is_weaker_than_verify_full(self) -> bool {
        self > Self::VerifyFull
    }

    /// The mode the drivers are actually given: everything at `verify-ca` or below
    /// is still driven through rustls' verification, because a connection that says
    /// `require` and then gets *no* verification is worse than either extreme.
    pub fn effective(self) -> Self {
        match self {
            Self::Prefer | Self::Require => Self::Require,
            other => other,
        }
    }
}

/// The non-secret facts of a connection string.
///
/// Every field is publishable: `db.status` shows `engine`, `database`,
/// `reader_user`, `tls` and `latency_ms`, and § 5.8 of `docs/transport.md` says
/// exactly that ("не секрет; пароль не отдаётся никогда").
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ConnFacts {
    pub engine: Engine,
    pub host: String,
    pub port: u16,
    pub database: String,
    pub user: String,
    pub ssl: SslMode,
    /// Path to the CA bundle, when the string names one. Not a secret: a public
    /// certificate.
    pub root_cert: Option<String>,
    /// Connect timeout in seconds, when the string names one.
    pub connect_timeout_secs: Option<u64>,
}

impl ConnFacts {
    /// One line that is safe to log: no password, no query string.
    pub fn redacted(&self) -> String {
        format!(
            "{}://{}@{}:{}/{} tls={}",
            self.engine.as_str(),
            self.user,
            self.host,
            self.port,
            self.database,
            self.ssl.as_str()
        )
    }

    /// Latency/`db.status` label. `tunnel` when the agent connected through an SSH
    /// forward to loopback, because "verify-full to 127.0.0.1" would be misleading
    /// about what is actually verified.
    pub fn tls_label(&self) -> &'static str {
        if is_loopback(&self.host) {
            "tunnel"
        } else {
            self.ssl.as_str()
        }
    }
}

fn is_loopback(host: &str) -> bool {
    host == "127.0.0.1" || host == "::1" || host == "localhost"
}

/// The URL without its query string.
///
/// Both drivers reject parameters they do not know (`postgres::Config` on
/// `sslmode=verify-full` is fine, on `ssl-ca` is not; `mysql::Opts` rejects anything
/// outside its own list), and the policy parameters are handled here anyway. Stripping
/// the query is therefore what makes the two parsers agree, and it keeps every secret
/// in the part of the string the driver parses.
pub fn base_url(url: &str) -> &str {
    match url.split_once('?') {
        Some((base, _query)) => base,
        None => url,
    }
}

/// Parse a connection string into [`ConnFacts`].
///
/// Returns [`Error::Database`] with [`crate::db::class::CONFIG`] for anything
/// malformed — never the input, which may contain the password.
pub fn parse(url: &str) -> Result<ConnFacts> {
    let trimmed = url.trim();
    if trimmed.is_empty() {
        return Err(Error::Database(crate::db::class::CONFIG));
    }
    let (scheme, rest) = trimmed
        .split_once("://")
        .ok_or(Error::Database(crate::db::class::CONFIG))?;
    let engine = match scheme.to_ascii_lowercase().as_str() {
        "postgres" | "postgresql" | "pg" => Engine::Postgres,
        "mysql" | "mariadb" => Engine::MySql,
        _ => return Err(Error::Database(crate::db::class::CONFIG)),
    };

    // query string first: it is the only part that can contain `?` inside a value
    let (authority_path, query) = match rest.split_once('?') {
        Some((head, query)) => (head, query),
        None => (rest, ""),
    };
    let (authority, database_path) = match authority_path.split_once('/') {
        Some((authority, path)) => (authority, path),
        None => (authority_path, ""),
    };
    // user:pass@host:port — `rsplit_once('@')` so a password may contain `@`.
    let (credentials, host_port) = match authority.rsplit_once('@') {
        Some((credentials, host_port)) => (Some(credentials), host_port),
        None => (None, authority),
    };

    let user = match credentials {
        Some(pair) => pair
            .split_once(':')
            .map(|(user, _pass)| user)
            .unwrap_or(pair),
        None => "",
    };
    if user.is_empty() {
        return Err(Error::Database(crate::db::class::CONFIG));
    }
    // The password is *not* copied out: the drivers get the whole string through
    // `credentials::load`, and nothing in this module keeps a second copy.
    if let Some(pair) = credentials {
        if pair
            .split_once(':')
            .map(|(_, pass)| pass.is_empty())
            .unwrap_or(true)
        {
            return Err(Error::Database(crate::db::class::CONFIG));
        }
    }

    let (host, port) = split_host_port(host_port, engine)?;
    let database = database_path.trim_matches('/').to_string();
    if database.is_empty() {
        return Err(Error::Database(crate::db::class::CONFIG));
    }

    let params = parse_query(query);
    let ssl_param = params
        .iter()
        .find(|(key, _)| matches!(key.as_str(), "sslmode" | "ssl-mode" | "ssl_mode"));
    let ssl = match ssl_param {
        None => SslMode::VerifyFull,
        Some((_, value)) => {
            SslMode::parse(value).ok_or(Error::Database(crate::db::class::CONFIG))?
        }
    };
    if ssl == SslMode::Disable {
        // § 2.6 п. 6: refusing is the policy, not a warning. A cleartext connection to
        // the DB would put the user's messages on the wire in the clear, and the
        // agent is on the same host precisely so this is never necessary.
        return Err(Error::Database(crate::db::class::INSECURE_TRANSPORT));
    }
    let root_cert = params
        .iter()
        .find(|(key, _)| {
            matches!(
                key.as_str(),
                "sslrootcert" | "ssl-ca" | "ssl_ca" | "sslrootcert_path"
            )
        })
        .map(|(_, value)| value.clone());
    let connect_timeout_secs = params
        .iter()
        .find(|(key, _)| matches!(key.as_str(), "connect_timeout" | "connect-timeout"))
        .and_then(|(_, value)| value.parse::<u64>().ok())
        .filter(|secs| *secs > 0 && *secs <= 300);

    Ok(ConnFacts {
        engine,
        host,
        port,
        database,
        user: user.to_string(),
        ssl,
        root_cert,
        connect_timeout_secs,
    })
}

fn split_host_port(host_port: &str, engine: Engine) -> Result<(String, u16)> {
    // `[::1]:5432` and `host:5432` and bare `host`.
    if let Some(rest) = host_port.strip_prefix('[') {
        let (host, tail) = rest
            .split_once(']')
            .ok_or(Error::Database(crate::db::class::CONFIG))?;
        let port = match tail.strip_prefix(':') {
            Some(port) => port
                .parse::<u16>()
                .map_err(|_| Error::Database(crate::db::class::CONFIG))?,
            None if tail.is_empty() => engine.default_port(),
            None => return Err(Error::Database(crate::db::class::CONFIG)),
        };
        return Ok((host.to_string(), port));
    }
    match host_port.rsplit_once(':') {
        Some((host, port)) => {
            if host.is_empty() || port.is_empty() {
                return Err(Error::Database(crate::db::class::CONFIG));
            }
            let port = port
                .parse::<u16>()
                .map_err(|_| Error::Database(crate::db::class::CONFIG))?;
            Ok((host.to_string(), port))
        }
        None => {
            if host_port.is_empty() {
                return Err(Error::Database(crate::db::class::CONFIG));
            }
            Ok((host_port.to_string(), engine.default_port()))
        }
    }
}

fn parse_query(query: &str) -> Vec<(String, String)> {
    query
        .split('&')
        .filter(|pair| !pair.is_empty())
        .filter_map(|pair| {
            let (key, value) = pair.split_once('=')?;
            Some((key.to_ascii_lowercase(), value.to_string()))
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::db::class;

    fn error_class(url: &str) -> &'static str {
        match parse(url) {
            Ok(facts) => panic!("expected a refusal, got {facts:?}"),
            Err(Error::Database(class)) => class,
            Err(other) => panic!("expected a database error, got {other}"),
        }
    }

    #[test]
    fn a_postgres_url_is_parsed_into_facts() {
        let facts = parse("postgresql://teleforge_agent:s3cret@db.example.com:5433/teleforge")
            .expect("a well-formed URL must parse");
        assert_eq!(facts.engine, Engine::Postgres);
        assert_eq!(facts.host, "db.example.com");
        assert_eq!(facts.port, 5433);
        assert_eq!(facts.database, "teleforge");
        assert_eq!(facts.user, "teleforge_agent");
        // verify-full is the default (§ 2.6 п. 6), not something the URL must say.
        assert_eq!(facts.ssl, SslMode::VerifyFull);
        assert_eq!(facts.tls_label(), "verify-full");
        assert!(facts.root_cert.is_none());
    }

    #[test]
    fn a_mysql_url_uses_the_engine_default_port_and_its_ssl_spelling() {
        let facts =
            parse("mysql://agent:pw@127.0.0.1/teleforge?ssl-mode=require&ssl-ca=/etc/ca.pem")
                .expect("a well-formed mysql URL must parse");
        assert_eq!(facts.engine, Engine::MySql);
        assert_eq!(facts.port, 3306);
        assert_eq!(facts.ssl, SslMode::Require);
        assert_eq!(facts.root_cert.as_deref(), Some("/etc/ca.pem"));
        // Loopback is reported as `tunnel`: what is verified is the SSH host key,
        // not the DB certificate.
        assert_eq!(facts.tls_label(), "tunnel");
    }

    #[test]
    fn the_password_never_appears_in_the_redacted_form() {
        let facts = parse("postgres://user:hunter2@db:5432/tf?sslmode=verify-ca").unwrap();
        let redacted = facts.redacted();
        assert!(!redacted.contains("hunter2"), "leaked: {redacted}");
        assert!(redacted.contains("user@db:5432/tf"));
        // `Debug` is what a `?`/`{:?}` in a log line would print.
        let debug = format!("{facts:?}");
        assert!(
            !debug.contains("hunter2"),
            "Debug leaked the password: {debug}"
        );
    }

    #[test]
    fn stronger_and_weaker_tls_modes_are_ordered() {
        assert!(!SslMode::VerifyFull.is_weaker_than_verify_full());
        assert!(SslMode::VerifyCa.is_weaker_than_verify_full());
        assert!(SslMode::Require.is_weaker_than_verify_full());
        assert!(SslMode::Prefer.is_weaker_than_verify_full());
        assert!(SslMode::Disable.is_weaker_than_verify_full());
        // Nothing is ever driven without verification.
        assert_eq!(SslMode::Prefer.effective(), SslMode::Require);
        assert_eq!(SslMode::VerifyCa.effective(), SslMode::VerifyCa);
    }

    #[test]
    fn every_spelling_of_a_known_mode_maps_to_one_variant() {
        for value in ["verify-full", "VERIFY_FULL", "verifyfull", " verify-full "] {
            assert_eq!(SslMode::parse(value), Some(SslMode::VerifyFull), "{value}");
        }
        for value in ["trust", "disable", "off", "none", "false"] {
            assert_eq!(SslMode::parse(value), Some(SslMode::Disable), "{value}");
        }
        assert_eq!(SslMode::parse("maybe"), None);
    }

    #[test]
    fn a_cleartext_connection_is_refused_not_downgraded() {
        assert_eq!(
            error_class("postgres://u:p@h/db?sslmode=disable"),
            class::INSECURE_TRANSPORT
        );
        assert_eq!(
            error_class("mysql://u:p@h/db?ssl-mode=trust"),
            class::INSECURE_TRANSPORT
        );
    }

    #[test]
    fn malformed_urls_are_refused_without_echoing_the_input() {
        for url in [
            "",
            "not a url",
            "mongodb://u:p@h/db",
            "postgres:///db",
            "postgres://user@host/db",  // no password
            "postgres://:pass@host/db", // no user
            "postgres://u:p@host/db?sslmode=maybe",
            "postgres://u:p@/db", // no host
            "postgres://u:p@host:notaport/db",
        ] {
            let class = error_class(url);
            assert!(
                class == class::CONFIG,
                "{url:?} must be a config error, got {class}"
            );
        }
    }

    #[test]
    fn the_error_message_cannot_carry_the_connection_string() {
        // The class is the whole message: a driver/parser message would be an
        // injection point for the password.
        let error = parse("postgres://u:hunter2@h/db?sslmode=maybe").unwrap_err();
        assert!(!error.to_string().contains("hunter2"));
        assert_eq!(error.to_string(), "database error: config");
    }

    #[test]
    fn ipv6_and_timeouts_are_handled() {
        let facts = parse("postgres://u:p@[::1]:5444/tf?connect_timeout=7").unwrap();
        assert_eq!(facts.host, "::1");
        assert_eq!(facts.port, 5444);
        assert_eq!(facts.connect_timeout_secs, Some(7));
        // An implausible timeout is ignored rather than honoured.
        let facts = parse("postgres://u:p@h/db?connect_timeout=99999").unwrap();
        assert_eq!(facts.connect_timeout_secs, None);
    }

    #[test]
    fn an_at_sign_in_the_password_does_not_shift_the_host() {
        let facts = parse("postgres://user:p@ss@db.example.com:5432/tf").unwrap();
        assert_eq!(facts.host, "db.example.com");
        assert_eq!(facts.user, "user");
    }

    #[test]
    fn the_base_url_keeps_every_secret_and_drops_every_policy_parameter() {
        // The drivers parse the base URL (so the password never enters a struct of
        // ours); the policy parameters are ours and would make them reject the URL.
        assert_eq!(
            base_url("postgres://u:p@h:5432/db?sslmode=verify-full&sslrootcert=/ca.pem"),
            "postgres://u:p@h:5432/db"
        );
        assert_eq!(base_url("mysql://u:p@h/db"), "mysql://u:p@h/db");
    }
}
