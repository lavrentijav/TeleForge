//! Wire protocol: methods, envelope, error codes, and the tiny JSON helpers the
//! std-only build needs.
//!
//! # Versioning rules
//!
//! * `protocol` is an integer. The agent accepts `MIN_SUPPORTED ..= PROTOCOL`
//!   and answers `1001 unsupported_protocol` outside that window, so a stale
//!   client gets a *diagnosable* error instead of a parse failure.
//! * Adding a field is not a version bump. Removing one, renaming one, or
//!   changing its type **is**.
//! * `request_id` is echoed verbatim, so a client can pipeline.
//! * `idempotency_key` makes `sync.push` and `tunnel.up` retry-safe: replaying the
//!   same key returns the stored result. `sync.push` additionally carries a
//!   per-op `op_id`, so even a *different* request that contains an already
//!   applied op is safe. That is the fix for the C++ retry behaviour, where
//!   `RunPgUpload` re-uploads everything and relies on `ON CONFLICT DO NOTHING`
//!   (which silently swallows genuine second edits — finding H9).
//!
//! # Examples
//!
//! Handshake:
//! ```json
//! {"protocol":1,"request_id":"01J9...","method":"hello",
//!  "params":{"client_version":"0.1.0","device_id":"a1b2..."},
//!  "idempotency_key":"01J9..."}
//! ```
//! ```json
//! {"protocol":1,"request_id":"01J9...","ok":true,
//!  "result":{"agent_version":"0.1.0","protocol":1,
//!            "min_client_version":"0.1.0","tdlib_version":"1.8.29",
//!            "vault":"locked","accounts":1,"high_water":1234}}
//! ```
//!
//! Push (one op shown):
//! ```json
//! {"protocol":1,"request_id":"01JA...","method":"sync.push",
//!  "idempotency_key":"01JA...",
//!  "params":{"tenant":"5f3c...","account":"9a1b...",
//!            "ops":[{"op_id":"9f0e...","kind":3,"dialog_id":12345,
//!                    "message_id":0,"sub":0,"field":32,
//!                    "hlc":"0000018f3c1a2b40-0000",
//!                    "writer_seq":7,
//!                    "tombstone":false,
//!                    "vclock":"dev-a:7,dev-b:2",
//!                    "value_ct":"base64..."}]}}
//! ```
//! ```json
//! {"protocol":1,"request_id":"01JA...","ok":true,
//!  "result":{"applied":1,"ignored":0,"duplicates":0,"rejected":0,
//!            "conflicts":0,"high_water":1235}}
//! ```
//!
//! Conflict, surfaced instead of silently resolved:
//! ```json
//! {"protocol":1,"request_id":"01JA...","ok":true,
//!  "result":{"applied":0,"ignored":0,"duplicates":0,"rejected":0,"conflicts":1,
//!            "conflict_ids":[41],
//!            "high_water":1240}}
//! ```

use crate::error::{Error, Result};

pub const PROTOCOL_VERSION: u16 = 1;
pub const MIN_SUPPORTED_PROTOCOL: u16 = 1;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Method {
    /// Version + state handshake. Allowed while locked.
    Hello,
    /// Redeem a pairing code for a device token. Allowed while locked.
    Pair,
    /// Unlock the vault with a passphrase. Needs a device token.
    Unlock,
    Lock,
    Status,
    /// Cheap "am I behind" probe; replaces `PgFetchRemoteStamp`.
    SyncStamp,
    SyncPull,
    SyncPush,
    /// List and resolve conflicts.
    SyncConflicts,
    /// Delete one account's history up to an `op_seq` the client has applied.
    SyncPrune,
    /// Is the agent connected to the user's database, how far behind is publication
    /// (§ 5.8). The client's "may I switch sync on" gate.
    DbStatus,
    /// Run one server-side DB ingest pass now. Internal: needs a token whose role is
    /// [`crate::auth::TokenRole::Ingest`], which `pair` never mints by default.
    DbIngest,
    TunnelUp,
    TunnelDown,
    TunnelStatus,
    Accounts,
    /// Start/stop a Telegram account inside TDLib.
    AccountStart,
    AccountStop,
    UpdateCheck,
    /// Shut the agent down (the client does this before replacing the binary,
    /// and the systemd unit is what actually restarts it).
    Shutdown,
}

impl Method {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Hello => "hello",
            Self::Pair => "pair",
            Self::Unlock => "unlock",
            Self::Lock => "lock",
            Self::Status => "status",
            Self::SyncStamp => "sync.stamp",
            Self::SyncPull => "sync.pull",
            Self::SyncPush => "sync.push",
            Self::SyncConflicts => "sync.conflicts",
            Self::SyncPrune => "sync.prune",
            Self::DbStatus => "db.status",
            Self::DbIngest => "db.ingest",
            Self::TunnelUp => "tunnel.up",
            Self::TunnelDown => "tunnel.down",
            Self::TunnelStatus => "tunnel.status",
            Self::Accounts => "accounts",
            Self::AccountStart => "account.start",
            Self::AccountStop => "account.stop",
            Self::UpdateCheck => "update.check",
            Self::Shutdown => "shutdown",
        }
    }

    pub fn parse(value: &str) -> Option<Self> {
        let all = [
            Self::Hello,
            Self::Pair,
            Self::Unlock,
            Self::Lock,
            Self::Status,
            Self::SyncStamp,
            Self::SyncPull,
            Self::SyncPush,
            Self::SyncConflicts,
            Self::SyncPrune,
            Self::DbStatus,
            Self::DbIngest,
            Self::TunnelUp,
            Self::TunnelDown,
            Self::TunnelStatus,
            Self::Accounts,
            Self::AccountStart,
            Self::AccountStop,
            Self::UpdateCheck,
            Self::Shutdown,
        ];
        all.into_iter().find(|method| method.as_str() == value)
    }

    /// Whether the method is allowed before `unlock`. Kept as a table so a new
    /// method must make an explicit decision instead of defaulting to "allowed".
    ///
    /// `db.status` is **not** here even though it answers with `vault: "locked"`:
    /// it requires the `sync:read` scope, and every scope implies an enrolled
    /// device. A locked vault is a normal state for `db.status`, not a permission.
    pub fn allowed_while_locked(self) -> bool {
        matches!(
            self,
            Self::Hello | Self::Pair | Self::Unlock | Self::Lock | Self::Status | Self::UpdateCheck
        )
    }

    /// Whether the method mutates state and therefore needs an idempotency key
    /// and the write scope.
    pub fn is_mutating(self) -> bool {
        matches!(
            self,
            Self::Pair
                | Self::Unlock
                | Self::Lock
                | Self::SyncPush
                | Self::TunnelUp
                | Self::TunnelDown
                | Self::AccountStart
                | Self::AccountStop
                | Self::Shutdown
                | Self::SyncConflicts
                | Self::SyncPrune
                // It writes `ops`, so a retry without a key would publish twice.
                | Self::DbIngest
        )
    }

    /// Required scope. `None` means "any authenticated device".
    pub fn scope(self) -> Option<crate::auth::Scope> {
        use crate::auth::Scope;
        match self {
            Self::Hello | Self::Status => Some(Scope::Status),
            Self::Pair | Self::Shutdown => Some(Scope::Admin),
            Self::Unlock | Self::Lock => Some(Scope::Unlock),
            Self::SyncStamp | Self::SyncPull | Self::SyncConflicts | Self::DbStatus => {
                Some(Scope::SyncRead)
            }
            Self::SyncPush | Self::SyncPrune => Some(Scope::SyncWrite),
            // § 15 п. 11: a client (reader) token must not be able to start the
            // ingest, so this is a scope `pair` never grants.
            Self::DbIngest => Some(Scope::DbIngest),
            Self::TunnelUp | Self::TunnelDown | Self::TunnelStatus => Some(Scope::Tunnel),
            Self::Accounts | Self::AccountStart | Self::AccountStop => Some(Scope::SyncWrite),
            Self::UpdateCheck => Some(Scope::Update),
        }
    }
}

/// Error codes. Stable integers, because the client's UI switches on them.
pub mod code {
    pub const INVALID_REQUEST: u32 = 1000;
    pub const UNSUPPORTED_PROTOCOL: u32 = 1001;
    pub const UNAUTHENTICATED: u32 = 1002;
    pub const FORBIDDEN: u32 = 1003;
    pub const VAULT_LOCKED: u32 = 1004;
    pub const NOT_FOUND: u32 = 1005;
    pub const CONFLICT: u32 = 1006;
    pub const RATE_LIMITED: u32 = 1007;
    pub const IDEMPOTENT_REPLAY: u32 = 1008;
    pub const TUNNEL_FAILED: u32 = 1009;
    pub const TDLIB_FAILED: u32 = 1010;
    pub const UPDATE_REJECTED: u32 = 1011;
    pub const INTERNAL: u32 = 1099;
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ApiError {
    pub code: u32,
    pub message: String,
    /// Whether the client may retry the *same* request with the same
    /// idempotency key.
    pub retryable: bool,
}

impl ApiError {
    pub fn new(code: u32, message: impl Into<String>, retryable: bool) -> Self {
        Self {
            code,
            message: message.into(),
            retryable,
        }
    }

    pub fn to_json(&self) -> String {
        format!(
            "{{\"code\":{},\"message\":\"{}\",\"retryable\":{}}}",
            self.code,
            json_escape(&self.message),
            self.retryable
        )
    }
}

impl From<&Error> for ApiError {
    fn from(error: &Error) -> Self {
        match error {
            Error::Auth(message) => Self::new(code::UNAUTHENTICATED, message.clone(), false),
            Error::Invalid(message) => Self::new(code::INVALID_REQUEST, message.clone(), false),
            Error::Protocol(message) => Self::new(code::INVALID_REQUEST, message.clone(), false),
            Error::Unsupported(message) => Self::new(code::INTERNAL, message.to_string(), false),
            Error::Crypto(message) => Self::new(code::INTERNAL, message.clone(), false),
            // A storage failure is ours, not the client's: retryable, because the
            // usual causes are `SQLITE_BUSY` and a full disk after a cleanup.
            Error::Storage(message) => Self::new(code::INTERNAL, message.clone(), true),
            // The user's DB is a *deployment* problem: retrying the same request
            // with the same parameters will not fix it, and the class is already
            // in `db.status.last_error` for the client to show.
            Error::Database(class) => {
                Self::new(code::INTERNAL, format!("database error: {class}"), false)
            }
            Error::Update(message) => Self::new(code::UPDATE_REJECTED, message.clone(), true),
            Error::Tls(message) => Self::new(code::INTERNAL, message.clone(), false),
            // Not retryable with the same parameters: the client must re-read
            // `sync.stamp` and start from what the server actually has.
            Error::Conflict(message) => Self::new(code::CONFLICT, message.clone(), false),
            Error::NotImplemented(what) => Self::new(
                code::INTERNAL,
                format!("not implemented in this build: {what}"),
                false,
            ),
            Error::Io(io) => Self::new(code::INTERNAL, io.to_string(), true),
        }
    }
}

/// Request envelope.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Envelope {
    pub protocol: u16,
    pub request_id: String,
    pub idempotency_key: String,
    pub method: Method,
    /// Raw JSON object. The agent parses the fields it needs with the helpers
    /// below; a real build swaps this for `serde_json::Value`.
    pub params: String,
}

impl Envelope {
    pub fn to_json(&self) -> String {
        format!(
            "{{\"protocol\":{},\"request_id\":\"{}\",\"idempotency_key\":\"{}\",\
             \"method\":\"{}\",\"params\":{}}}",
            self.protocol,
            json_escape(&self.request_id),
            json_escape(&self.idempotency_key),
            self.method.as_str(),
            if self.params.trim().is_empty() {
                "{}".to_string()
            } else {
                self.params.clone()
            }
        )
    }

    /// Reject envelopes the agent cannot honour, before any work is done.
    pub fn validate(&self) -> Result<()> {
        if self.protocol < MIN_SUPPORTED_PROTOCOL || self.protocol > PROTOCOL_VERSION {
            return Err(Error::Protocol(format!(
                "protocol {} is outside {}..={}",
                self.protocol, MIN_SUPPORTED_PROTOCOL, PROTOCOL_VERSION
            )));
        }
        if self.request_id.is_empty() || self.request_id.len() > 64 {
            return Err(Error::Protocol(
                "request_id must be 1..=64 chars".to_string(),
            ));
        }
        if self.method.is_mutating() && self.idempotency_key.is_empty() {
            return Err(Error::Protocol(format!(
                "{} requires an idempotency_key",
                self.method.as_str()
            )));
        }
        Ok(())
    }
}

/// Response envelope.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Response {
    pub protocol: u16,
    pub request_id: String,
    pub ok: bool,
    pub result: String,
    pub error: Option<ApiError>,
}

impl Response {
    pub fn success(request_id: &str, result: String) -> Self {
        Self {
            protocol: PROTOCOL_VERSION,
            request_id: request_id.to_string(),
            ok: true,
            result,
            error: None,
        }
    }

    pub fn failure(request_id: &str, error: ApiError) -> Self {
        Self {
            protocol: PROTOCOL_VERSION,
            request_id: request_id.to_string(),
            ok: false,
            result: "{}".to_string(),
            error: Some(error),
        }
    }

    pub fn to_json(&self) -> String {
        let error = match &self.error {
            Some(error) => error.to_json(),
            None => "null".to_string(),
        };
        format!(
            "{{\"protocol\":{},\"request_id\":\"{}\",\"ok\":{},\"result\":{},\"error\":{}}}",
            self.protocol,
            json_escape(&self.request_id),
            self.ok,
            if self.result.trim().is_empty() {
                "{}".to_string()
            } else {
                self.result.clone()
            },
            error
        )
    }
}

/// Escape a string for a JSON literal.
pub fn json_escape(input: &str) -> String {
    let mut out = String::with_capacity(input.len() + 2);
    for ch in input.chars() {
        match ch {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if (c as u32) < 0x20 => out.push_str(&format!("\\u{:04x}", c as u32)),
            c => out.push(c),
        }
    }
    out
}

/// Minimal JSON member readers.
///
/// These exist so the std-only skeleton can parse `meta.json` and the envelope
/// method name without a dependency. They are *not* a JSON parser: they find a
/// top-level `"key"` and read the literal after it. A real build uses
/// `serde_json`, and `protocol::tests` pins the behaviour that matters (quoted
/// strings and unsigned integers) so the replacement is verifiable.
pub fn extract_str(json: &str, key: &str) -> Option<String> {
    let start = find_key(json, key)?;
    let rest = &json[start..];
    // The value must *start* with a quote. Without this check a `null` or numeric
    // value makes `rest.find('"')` jump to the next member's key, so
    // `extract_str(json, "optional")` returns `"the_next_key"` — which is how a
    // `"wrapped_dek_recovery":null` field turned into "bad hex digit 'v'".
    let value = rest.trim_start();
    if !value.starts_with('"') {
        return None;
    }
    let mut out = String::new();
    let mut escaped = false;
    for ch in value[1..].chars() {
        if escaped {
            match ch {
                'n' => out.push('\n'),
                'r' => out.push('\r'),
                't' => out.push('\t'),
                'u' => {
                    // Leave \uXXXX un-decoded: only ASCII members matter here.
                    return None;
                }
                other => out.push(other),
            }
            escaped = false;
            continue;
        }
        match ch {
            '\\' => escaped = true,
            '"' => return Some(out),
            other => out.push(other),
        }
    }
    None
}

pub fn extract_number(json: &str, key: &str) -> Option<u64> {
    let start = find_key(json, key)?;
    let rest = &json[start..];
    let mut digits = String::new();
    let mut started = false;
    for ch in rest.chars() {
        if ch.is_ascii_digit() {
            digits.push(ch);
            started = true;
        } else if started {
            break;
        } else if ch == '-' {
            // Negative numbers are not used by the protocol; refuse rather than
            // wrap into a huge u64.
            return None;
        } else if ch == ' ' || ch == '\t' || ch == ':' {
            continue;
        } else {
            return None;
        }
    }
    digits.parse::<u64>().ok()
}

pub fn extract_bool(json: &str, key: &str) -> Option<bool> {
    let start = find_key(json, key)?;
    let rest = json[start..].trim_start_matches([' ', '\t', ':', '"']);
    if rest.starts_with("true") {
        Some(true)
    } else if rest.starts_with("false") {
        Some(false)
    } else {
        None
    }
}

/// Position just after `"key"` and its `:`.
fn find_key(json: &str, key: &str) -> Option<usize> {
    let needle = format!("\"{key}\"");
    let at = json.find(&needle)?;
    let after = at + needle.len();
    let rest = &json[after..];
    let colon = rest.find(':')?;
    Some(after + colon + 1)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn method_roundtrip() {
        for name in [
            "hello",
            "pair",
            "unlock",
            "sync.stamp",
            "sync.pull",
            "sync.push",
            "tunnel.up",
            "update.check",
        ] {
            let method = Method::parse(name).expect("known method");
            assert_eq!(method.as_str(), name);
        }
        assert!(Method::parse("nope").is_none());
    }

    #[test]
    fn locked_allow_list_is_explicit() {
        assert!(Method::Hello.allowed_while_locked());
        assert!(Method::UpdateCheck.allowed_while_locked());
        assert!(!Method::SyncPull.allowed_while_locked());
        assert!(!Method::TunnelUp.allowed_while_locked());
        // `db.status` answers with `vault: "locked"`, but it still needs a token: a
        // locked vault is a state of the vault, not a permission to read the DB path.
        assert!(!Method::DbStatus.allowed_while_locked());
    }

    #[test]
    fn the_db_methods_have_the_documented_scope_and_shape() {
        assert_eq!(Method::DbStatus.as_str(), "db.status");
        assert_eq!(Method::DbIngest.as_str(), "db.ingest");
        assert_eq!(Method::parse("db.status"), Some(Method::DbStatus));
        assert_eq!(Method::parse("db.ingest"), Some(Method::DbIngest));

        // § 5.8: the device that pulls reads the status; it changes nothing.
        assert_eq!(Method::DbStatus.scope(), Some(crate::auth::Scope::SyncRead));
        assert!(!Method::DbStatus.is_mutating());

        // § 15 п. 11: starting the ingest is a *separate* grant, and it writes `ops`,
        // so it needs an idempotency key like any other mutating method.
        assert_eq!(Method::DbIngest.scope(), Some(crate::auth::Scope::DbIngest));
        assert!(Method::DbIngest.is_mutating());
        assert!(Envelope {
            protocol: PROTOCOL_VERSION,
            request_id: "r1".to_string(),
            idempotency_key: String::new(),
            method: Method::DbIngest,
            params: "{}".to_string(),
        }
        .validate()
        .is_err());
    }

    #[test]
    fn mutating_methods_need_idempotency() {
        let envelope = Envelope {
            protocol: PROTOCOL_VERSION,
            request_id: "r1".to_string(),
            idempotency_key: String::new(),
            method: Method::SyncPush,
            params: "{}".to_string(),
        };
        assert!(envelope.validate().is_err());
        let read_only = Envelope {
            method: Method::SyncPull,
            ..envelope.clone()
        };
        assert!(read_only.validate().is_ok());
    }

    #[test]
    fn protocol_window_is_enforced() {
        let bad = Envelope {
            protocol: 99,
            request_id: "r".to_string(),
            idempotency_key: "k".to_string(),
            method: Method::Hello,
            params: "{}".to_string(),
        };
        match bad.validate() {
            Err(Error::Protocol(message)) => assert!(message.contains("outside")),
            other => panic!("expected protocol error, got {other:?}"),
        }
    }

    #[test]
    fn envelope_json_is_well_formed() {
        let envelope = Envelope {
            protocol: 1,
            request_id: "req-1".to_string(),
            idempotency_key: "idem-1".to_string(),
            method: Method::SyncPull,
            params: "{\"since_seq\":5}".to_string(),
        };
        let json = envelope.to_json();
        assert!(json.contains("\"method\":\"sync.pull\""));
        assert!(json.contains("\"params\":{\"since_seq\":5}"));
        assert!(json.starts_with('{') && json.ends_with('}'));
    }

    #[test]
    fn extraction_handles_escapes_and_numbers() {
        let json = "{\"a\":\"line\\nbreak\",\"n\":1234,\"flag\":true,\"nul\":null}";
        assert_eq!(extract_str(json, "a").unwrap(), "line\nbreak");
        assert_eq!(extract_number(json, "n"), Some(1234));
        assert_eq!(extract_bool(json, "flag"), Some(true));
        assert_eq!(extract_str(json, "nul"), None);
        assert_eq!(extract_number(json, "missing"), None);
    }

    #[test]
    fn response_json_carries_error_or_null() {
        let ok = Response::success("r", "{\"x\":1}".to_string());
        assert!(ok.to_json().contains("\"error\":null"));
        let bad = Response::failure("r", ApiError::new(code::VAULT_LOCKED, "locked", false));
        let json = bad.to_json();
        assert!(json.contains("\"ok\":false"));
        assert!(json.contains("\"code\":1004"));
    }

    #[test]
    fn errors_map_to_stable_codes() {
        let locked = ApiError::from(&Error::Auth("x".to_string()));
        assert_eq!(locked.code, code::UNAUTHENTICATED);
        let invalid = ApiError::from(&Error::Invalid("x".to_string()));
        assert_eq!(invalid.code, code::INVALID_REQUEST);
        let unimplemented = ApiError::from(&Error::NotImplemented("argon2"));
        assert_eq!(unimplemented.code, code::INTERNAL);
        assert!(unimplemented.message.contains("argon2"));
    }

    #[test]
    fn extract_str_returns_none_for_a_null_or_numeric_value() {
        // Regression: `rest.find('"')` used to skip past `null` and return the
        // *next* member's key, which made optional envelope fields parse as hex.
        let json = r#"{"optional":null,"number":7,"name":"value","after":"x"}"#;
        assert_eq!(extract_str(json, "optional"), None);
        assert_eq!(extract_str(json, "number"), None);
        assert_eq!(extract_str(json, "name").as_deref(), Some("value"));
        assert_eq!(extract_str(json, "missing"), None);
        // Whitespace between the colon and the value is still fine.
        assert_eq!(extract_str(r#"{"k": "v"}"#, "k").as_deref(), Some("v"));
    }

    #[test]
    fn extract_number_and_bool_ignore_other_types() {
        let json = r#"{"a":1,"b":"2","c":true,"d":null}"#;
        assert_eq!(extract_number(json, "a"), Some(1));
        assert_eq!(extract_number(json, "b"), None);
        assert_eq!(extract_number(json, "d"), None);
        assert_eq!(extract_bool(json, "c"), Some(true));
        assert_eq!(extract_bool(json, "a"), None);
    }
}
