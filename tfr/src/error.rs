//! One error enum for the whole crate.
//!
//! Deliberately not a thiserror/anyhow tree yet: the variants below are the ones
//! that must map to distinct agent exit codes and to distinct protocol error
//! codes, and keeping that mapping in one file makes the mapping auditable.

use std::fmt;

#[derive(Debug)]
pub enum Error {
    /// A primitive that this skeleton intentionally leaves unimplemented.
    /// Carries the crate that will fill it in, so the message is actionable.
    NotImplemented(&'static str),
    /// Caller passed something malformed.
    Invalid(String),
    /// The host does not allow this operation (e.g. `/proc` hardening absent).
    Unsupported(&'static str),
    Io(std::io::Error),
    Crypto(String),
    Auth(String),
    Protocol(String),
    /// SQLite / on-disk store failure. Carries the message, not the error type,
    /// so `error.rs` stays independent of the database crate.
    Storage(String),
    /// The user's PostgreSQL/MySQL: connect, TLS, authentication or a read failed.
    ///
    /// Carries a *stable class* (`db::class::*`) — `connect`, `auth`, `query`,
    /// `insecure_transport`, `vault_locked`, … — and never the driver's own
    /// message: driver errors can echo the connection string (which contains the
    /// password) or a value from the row being read, and both would then reach
    /// `db.status`, the log, or a client. The class is what `db.status.last_error`
    /// publishes (§ 5.8) and what the ingest loop logs.
    Database(&'static str),
    Update(String),
    /// TLS configuration or handshake failure. Distinct from [`Self::Protocol`]
    /// because it happens *under* the protocol and must map to 400/500, not to a
    /// JSON-RPC code the client could retry.
    Tls(String),
    /// The caller's view of the account is inconsistent with ours: a cursor beyond
    /// `high_water`, or a store version the client cannot be talking about. Maps
    /// to HTTP 409 / code 1006. Never retried with the same parameters.
    Conflict(String),
}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::NotImplemented(what) => {
                write!(f, "not implemented in the std-only skeleton: {what}")
            }
            Self::Invalid(msg) => write!(f, "invalid input: {msg}"),
            Self::Unsupported(what) => write!(f, "unsupported on this host: {what}"),
            Self::Io(e) => write!(f, "io error: {e}"),
            Self::Crypto(msg) => write!(f, "crypto error: {msg}"),
            Self::Auth(msg) => write!(f, "auth error: {msg}"),
            Self::Protocol(msg) => write!(f, "protocol error: {msg}"),
            Self::Storage(msg) => write!(f, "storage error: {msg}"),
            Self::Database(class) => write!(f, "database error: {class}"),
            Self::Update(msg) => write!(f, "update error: {msg}"),
            Self::Tls(msg) => write!(f, "tls error: {msg}"),
            Self::Conflict(msg) => write!(f, "conflict: {msg}"),
        }
    }
}

impl From<rusqlite::Error> for Error {
    fn from(value: rusqlite::Error) -> Self {
        Self::Storage(value.to_string())
    }
}

impl std::error::Error for Error {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        match self {
            Self::Io(e) => Some(e),
            _ => None,
        }
    }
}

impl From<std::io::Error> for Error {
    fn from(value: std::io::Error) -> Self {
        Self::Io(value)
    }
}

pub type Result<T> = std::result::Result<T, Error>;

/// Stable process exit codes. The client's installer reads these over SSH, so
/// they must never be renumbered.
pub mod exit {
    pub const OK: i32 = 0;
    pub const USAGE: i32 = 2;
    pub const LOCKED: i32 = 3;
    pub const AUTH: i32 = 4;
    pub const CONFIG: i32 = 5;
    pub const UPDATE_FAILED: i32 = 6;
    pub const SELF_TEST_FAILED: i32 = 7;
    pub const INTERNAL: i32 = 70;
}

impl Error {
    /// Process exit code for a top-level failure.
    pub fn exit_code(&self) -> i32 {
        match self {
            Self::Auth(_) => exit::AUTH,
            Self::Invalid(_) => exit::USAGE,
            Self::Update(_) => exit::UPDATE_FAILED,
            Self::Unsupported(_) => exit::SELF_TEST_FAILED,
            // Everything else, including [`Self::Database`]: an unreachable
            // PostgreSQL/MySQL is not a *process* failure — `serve` must keep
            // answering `status` and `sync.pull` from the local journal, and the
            // ingest loop is what handles the error.
            _ => exit::INTERNAL,
        }
    }
}
