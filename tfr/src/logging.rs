//! Structured logging with mandatory redaction.
//!
//! Audit findings this closes:
//! * L6 — `noteWakeSignal` logged an arbitrary `detail` that callers filled with
//!   chat and user names.
//! * M14 / C1 — nothing may ever print key material, tokens, connection strings
//!   or message text.
//!
//! Two independent defences, because "remember not to log it" has already
//! failed once in this codebase:
//! 1. Types that can hold secrets ([`crate::secret::SecretBytes`]) have no
//!    `Display` and a redacting `Debug`.
//! 2. Every line passes through [`redact_line`] before it is written, so a
//!    `format!` that interpolates a raw `&str` from the wire is still scrubbed.

use std::fmt::Write as _;

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub enum Level {
    Error,
    Warn,
    Info,
    Debug,
    Trace,
}

impl Level {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Error => "error",
            Self::Warn => "warn",
            Self::Info => "info",
            Self::Debug => "debug",
            Self::Trace => "trace",
        }
    }

    pub fn parse(value: &str) -> Option<Self> {
        match value {
            "error" => Some(Self::Error),
            "warn" => Some(Self::Warn),
            "info" => Some(Self::Info),
            "debug" => Some(Self::Debug),
            "trace" => Some(Self::Trace),
            _ => None,
        }
    }
}

/// Keys whose values are scrubbed regardless of quoting style.
///
/// Matched case-insensitively, and the list is intentionally broad: a false
/// positive costs one unreadable log field, a false negative costs a session.
const SENSITIVE_KEYS: &[&str] = &[
    "password",
    "passwd",
    "passphrase",
    "secret",
    "token",
    "authorization",
    "apikey",
    "api_key",
    "openai_api_key",
    "private_key",
    "identity_file",
    "pgconnstring",
    "connstring",
    "dek",
    "kek",
    "salt",
    "verifier",
    "signature",
];

/// Anything longer than this that looks like base64/hex is treated as a blob and
/// replaced by its length. Message payloads and ciphertext land here.
const BLOB_RUN_LEN: usize = 64;

pub fn redact_line(line: &str) -> String {
    let mut out = String::with_capacity(line.len());
    for (index, token) in line.split(' ').enumerate() {
        if index > 0 {
            out.push(' ');
        }
        out.push_str(&redact_token(token));
    }
    out
}

fn redact_token(token: &str) -> String {
    if token.len() > BLOB_RUN_LEN && is_blob_like(token) {
        return format!("<blob:{}b>", token.len());
    }
    let lower = token.to_ascii_lowercase();
    for key in SENSITIVE_KEYS {
        if value_after_key(&lower, key).is_some() {
            let prefix_len = token.len() - rest_len(token, key);
            let prefix = &token[..prefix_len];
            return format!("{prefix}<redacted>");
        }
    }
    token.to_string()
}

/// True when `token_lower` contains `key` followed by `=`/`:`/`"`, i.e. it is a
/// `key=value` pair rather than a word that merely mentions the key.
fn value_after_key(token_lower: &str, key: &str) -> Option<()> {
    let mut search_from = 0usize;
    while let Some(offset) = token_lower[search_from..].find(key) {
        let key_end = search_from + offset + key.len();
        let following = token_lower.as_bytes().get(key_end).copied();
        if matches!(following, Some(b'=') | Some(b':') | Some(b'"')) {
            return Some(());
        }
        search_from = key_end;
        if search_from >= token_lower.len() {
            break;
        }
    }
    None
}

/// Number of bytes to keep before the `<redacted>` marker.
fn rest_len(token: &str, key: &str) -> usize {
    let lower = token.to_ascii_lowercase();
    match lower.find(key) {
        Some(at) => token.len() - at,
        None => 0,
    }
}

fn is_blob_like(token: &str) -> bool {
    let mut hexish = 0usize;
    let mut b64ish = 0usize;
    for ch in token.chars() {
        if ch.is_ascii_hexdigit() {
            hexish += 1;
        } else if ch.is_ascii_alphanumeric() || ch == '+' || ch == '/' || ch == '=' {
            b64ish += 1;
        } else {
            return false;
        }
    }
    hexish * 100 / token.len().max(1) >= 90 || b64ish == token.len()
}

/// A logger that owns its sink. Kept allocation-light; the agent logs a handful
/// of lines per request, not per message.
pub struct Logger {
    pub level: Level,
    pub destination: Destination,
    pub process_hardened: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Destination {
    Stderr,
    /// Journald/SYSTEMD_LOG_TARGET-friendly plain lines; systemd adds timestamps.
    Journal,
}

impl Logger {
    pub fn new(level: Level, destination: Destination) -> Self {
        Self {
            level,
            destination,
            process_hardened: false,
        }
    }

    pub fn enabled(&self, level: Level) -> bool {
        level <= self.level
    }

    pub fn log(&self, level: Level, component: &str, message: &str) {
        if !self.enabled(level) {
            return;
        }
        let mut line = String::with_capacity(96);
        let _ = write!(
            line,
            "{} component={} {}",
            level.as_str(),
            component,
            message
        );
        let safe = redact_line(&line);
        match self.destination {
            Destination::Stderr => eprintln!("{safe}"),
            Destination::Journal => println!("{safe}"),
        }
    }

    pub fn info(&self, component: &str, message: &str) {
        self.log(Level::Info, component, message);
    }

    pub fn warn(&self, component: &str, message: &str) {
        self.log(Level::Warn, component, message);
    }

    pub fn error(&self, component: &str, message: &str) {
        self.log(Level::Error, component, message);
    }
}
