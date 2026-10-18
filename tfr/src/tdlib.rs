//! TDLib host: one client per Telegram account, driven through the C ABI.
//!
//! # Linking
//!
//! The agent links TDLib as a library, not as a subprocess, because TDLib's own
//! database lives inside the account directory this module has to encrypt and
//! because the client's sync engine needs the update stream, not a JSON relay.
//!
//! * **Static, preferred.** `libtdjson.a` (built with `-DTD_ENABLE_LTO=OFF` for
//!   musl) is linked with `#[link(name = "tdjson", kind = "static")]` behind the
//!   `tdlib-link` feature. A static link keeps the "single musl binary uploaded
//!   over SSH" promise: no `libtdjson.so` to place, no `LD_LIBRARY_PATH` trap, no
//!   version skew between the binary and the shared object on the VPS.
//! * **Dynamic, fallback.** `kind = "dylib"` for the Windows and macOS builds and
//!   for a distro package. In that mode `self-test` must find the library and
//!   report its version, and the updater must ship the matching `.so`.
//!
//! Because a static TDLib adds roughly 15-25 MiB to the binary and the client
//! uploads it over the user's SSH connection, the release also publishes a
//! `-slim` variant without TDLib for hosts where the user already runs a TDLib.
//! The client picks a variant from the manifest's `variants` list.
//!
//! # C ABI surface
//!
//! These are the real entry points of `td/telegram/td_json_client.h`:
//! `td_json_client_create`, `td_json_client_send`, `td_json_client_receive`,
//! `td_json_client_execute`, `td_json_client_destroy`, plus the simpler
//! `td_create_client_id` / `td_send` / `td_receive` / `td_execute` family and
//! `td_set_log_message_callback` / `td_set_log_verbosity_level`.
//!
//! Only `td_json_client_*` are used: the `td_*` family multiplexes all clients
//! through one message queue, which makes per-account shutdown and backpressure
//! much harder to reason about.
//!
//! # Encrypted TDLib storage
//!
//! `setTdlibParameters` accepts `database_encryption_key` (a byte array). The
//! agent sets it to `subkey(DEK, LABEL_TDLIB_DB)`, so TDLib encrypts its own
//! `db.sqlite`/`key`/`binlog` files. This is the single most important
//! difference from the C++ client, where the account's `tdata` is encrypted by
//! `lib_storage` but TeleForge's *own* `data.tforge` is not — finding M14.
//!
//! The key is never written to disk; it exists only between `unlock` and `lock`.
//! Consequence: a locked agent cannot serve Telegram, which is the intended
//! trade-off (the alternative is a key on disk, i.e. no encryption at rest).

use crate::error::{Error, Result};

/// Lowest TDLib the agent accepts. Below this the `database_encryption_key`
/// parameter and the `messageSendAck`/`updateMessageSendSucceeded` semantics the
/// sync engine depends on are not both present.
pub const MIN_TDLIB_VERSION: &str = "1.8.0";

/// Parameters handed to TDLib. Mirrors the subset of `setTdlibParameters` the
/// agent needs; the rest are defaults.
#[derive(Clone, Debug)]
pub struct TdlibParameters {
    pub database_directory: String,
    pub files_directory: String,
    pub use_message_database: bool,
    pub use_secret_chats: bool,
    pub use_chat_info_database: bool,
    pub system_language_code: String,
    pub device_model: String,
    pub system_version: String,
    pub application_version: String,
    /// `api_id`/`api_hash` belong to the *TeleForge* application, not the user's.
    pub api_id: i64,
    pub api_hash: String,
    pub enable_storage_optimizer: bool,
}

impl TdlibParameters {
    /// Build the `setTdlibParameters` JSON. `database_encryption_key` is passed as
    /// a hex string because the JSON binding expects a byte array encoded that
    /// way; the plaintext key never appears in the JSON we log (we never log it).
    pub fn to_set_parameters_json(&self, tdlib_key: &crate::secret::SecretBytes) -> String {
        format!(
            concat!(
                "{{\"@type\":\"setTdlibParameters\",\"parameters\":{{",
                "\"database_directory\":\"{}\",\"files_directory\":\"{}\",",
                "\"use_message_database\":{},\"use_secret_chats\":{},",
                "\"use_chat_info_database\":{},\"system_language_code\":\"{}\",",
                "\"device_model\":\"{}\",\"system_version\":\"{}\",",
                "\"application_version\":\"{}\",\"api_id\":{},\"api_hash\":\"{}\",",
                "\"enable_storage_optimizer\":{},\"database_encryption_key\":\"{}\"}}}}"
            ),
            json_escape(&self.database_directory),
            json_escape(&self.files_directory),
            self.use_message_database,
            self.use_secret_chats,
            self.use_chat_info_database,
            json_escape(&self.system_language_code),
            json_escape(&self.device_model),
            json_escape(&self.system_version),
            json_escape(&self.application_version),
            self.api_id,
            json_escape(&self.api_hash),
            self.enable_storage_optimizer,
            crate::crypto::sig::to_hex(tdlib_key.expose())
        )
    }
}

/// Per-account TDLib instance.
///
/// Not `Clone`, not `Send` by default: TDLib's JSON client is bound to the thread
/// that calls `receive`, so the owner keeps it on one dedicated thread and talks
/// to it through a channel. That single-owner discipline is the direct answer to
/// finding C5, where the C++ code touched one SQLite connection from a worker.
pub struct TdlibHost {
    pub account_id: [u8; 16],
    pub client: TdJsonClient,
    pub tdlib_version: String,
    pub authorized: bool,
}

/// Opaque handle. `*mut c_void` rather than a typed zero-sized struct keeps the
/// FFI surface honest without pulling in `bindgen`.
pub type TdJsonClient = *mut core::ffi::c_void;

impl TdlibHost {
    pub fn new(account_id: [u8; 16], tdlib_version: String, client: TdJsonClient) -> Self {
        Self {
            account_id,
            client,
            tdlib_version,
            authorized: false,
        }
    }

    /// Send one request. `td_json_client_send` is thread-safe per TDLib's docs,
    /// but the agent still funnels through one owner to keep ordering.
    pub fn send(&self, _request_json: &str) -> Result<()> {
        if self.client.is_null() {
            return Err(Error::Protocol("tdlib client is not created".to_string()));
        }
        // TODO(tdlib-link): CString::new(request_json)? then
        // `td_json_client_send(self.client, c_str.as_ptr())`.
        Err(Error::NotImplemented("td_json_client_send"))
    }

    /// Blocking receive with TDLib's own timeout (seconds, `0` = non-blocking).
    /// Returns `None` on timeout, which is the normal case in an idle poll loop.
    pub fn receive(&self, timeout_secs: f64) -> Result<Option<String>> {
        if self.client.is_null() {
            return Err(Error::Protocol("tdlib client is not created".to_string()));
        }
        let _ = timeout_secs;
        // TODO(tdlib-link): `td_json_client_receive` returns a `const char*` that
        // is valid only until the next call on this client; copy it into a
        // `CStr` → `String` *immediately*.
        Err(Error::NotImplemented("td_json_client_receive"))
    }

    pub fn destroy(mut self) {
        if !self.client.is_null() {
            // TODO(tdlib-link): td_json_client_destroy(self.client).
            self.client = core::ptr::null_mut();
        }
    }
}

impl Drop for TdlibHost {
    fn drop(&mut self) {
        if !self.client.is_null() {
            // Keep the drop path safe even if `destroy` already ran via mem::forget.
            self.client = core::ptr::null_mut();
        }
    }
}

/// Compare TDLib's reported `version` option against [`MIN_TDLIB_VERSION`].
///
/// Implemented here (not stubbed) because it is pure logic and because getting it
/// wrong is a security-relevant mistake: a lexicographic compare would rank
/// `1.10.0` below `1.9.0` and reject a perfectly good library.
pub fn check_version(reported: &str) -> Result<()> {
    let actual = parse_version(reported)?;
    let minimum = parse_version(MIN_TDLIB_VERSION)?;
    if actual < minimum {
        return Err(Error::Unsupported(
            "TDLib is older than the minimum this agent supports",
        ));
    }
    Ok(())
}

/// `"1.8.29"` → `(1, 8, 29)`. Tolerates a `-suffix` and ignores trailing fields.
pub fn parse_version(text: &str) -> Result<(u32, u32, u32)> {
    let core = text.trim().split(['-', '+']).next().unwrap_or("").trim();
    let mut parts = core.split('.');
    let major = parts
        .next()
        .and_then(|p| p.parse::<u32>().ok())
        .ok_or_else(|| Error::Invalid(format!("bad version {text:?}")))?;
    let minor = parts
        .next()
        .and_then(|p| p.parse::<u32>().ok())
        .unwrap_or(0);
    let patch = parts
        .next()
        .and_then(|p| p.parse::<u32>().ok())
        .unwrap_or(0);
    Ok((major, minor, patch))
}

/// Which auth state TDLib is in, derived from `updateAuthorizationState`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AuthorizationState {
    Closed,
    WaitTdlibParameters,
    WaitEncryptionKey,
    WaitPhoneNumber,
    WaitCode,
    WaitPassword,
    WaitRegistration,
    Ready,
    LoggingOut,
    Closing,
}

impl AuthorizationState {
    pub fn from_type_name(name: &str) -> Option<Self> {
        match name {
            "authorizationStateClosed" => Some(Self::Closed),
            "authorizationStateWaitTdlibParameters" => Some(Self::WaitTdlibParameters),
            "authorizationStateWaitEncryptionKey" => Some(Self::WaitEncryptionKey),
            "authorizationStateWaitPhoneNumber" => Some(Self::WaitPhoneNumber),
            "authorizationStateWaitCode" => Some(Self::WaitCode),
            "authorizationStateWaitPassword" => Some(Self::WaitPassword),
            "authorizationStateWaitRegistration" => Some(Self::WaitRegistration),
            "authorizationStateReady" => Some(Self::Ready),
            "authorizationStateLoggingOut" => Some(Self::LoggingOut),
            "authorizationStateClosing" => Some(Self::Closing),
            _ => None,
        }
    }

    /// Only `Ready` may serve sync reads; every other state means "the session is
    /// not usable", which the client renders as "требуется вход".
    pub fn can_serve_sync(self) -> bool {
        matches!(self, Self::Ready)
    }
}

/// A request the agent sends to TDLib. Kept as an enum rather than free-form JSON
/// so that every request the agent can issue is visible in one place — the C++
/// side builds JSON at the call site, which is how the sync code ended up with
/// positional placeholder arguments in `teleforge_sync_files.cpp`.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum TdRequest {
    GetVersion,
    SetTdlibParameters,
    CheckDatabaseEncryptionKey,
    GetMe,
    Close,
    /// Request the message history window the sync engine needs.
    GetChatHistory {
        chat_id: i64,
        from_message_id: i64,
        limit: i32,
    },
}

impl TdRequest {
    /// Method name for `getOption`, used by `self-test` to record the version.
    pub fn json(&self) -> Result<String> {
        match self {
            Self::GetVersion => Ok("{\"@type\":\"getOption\",\"name\":\"version\"}".to_string()),
            Self::CheckDatabaseEncryptionKey => {
                Ok("{\"@type\":\"checkDatabaseEncryptionKey\"}".to_string())
            }
            Self::GetMe => Ok("{\"@type\":\"getMe\"}".to_string()),
            Self::Close => Ok("{\"@type\":\"close\"}".to_string()),
            Self::SetTdlibParameters => Err(Error::Invalid(
                "setTdlibParameters needs an encryption key; use TdlibParameters::to_set_parameters_json"
                    .to_string(),
            )),
            Self::GetChatHistory {
                chat_id,
                from_message_id,
                limit,
            } => Ok(format!(
                "{{\"@type\":\"getChatHistory\",\"chat_id\":{chat_id},\
                 \"from_message_id\":{from_message_id},\"offset\":0,\"limit\":{limit},\
                 \"only_local\":false}}"
            )),
        }
    }
}

/// Guard against the TDLib log callback receiving message text.
///
/// TDLib logs at verbosity 1 (`fatal`+`error`) only, and the callback in a real
/// build must scrub before writing — the same rule as [`crate::logging`]. The
/// function is here, not in `logging`, because it also has to drop the
/// `LOG_INFO`-level lines that TDLib emits with chat titles in them.
pub fn log_verbosity_level() -> i32 {
    1
}

fn json_escape(input: &str) -> String {
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

// ---------------------------------------------------------------------------
// Raw FFI. Compiled only with `--features tdlib-link`, so `cargo check` on a
// machine without TDLib (and `cargo check --offline`) still succeeds.
// ---------------------------------------------------------------------------
#[cfg(feature = "tdlib-link")]
mod ffi {
    use super::TdJsonClient;
    use core::ffi::{c_char, c_double, c_int};

    #[link(name = "tdjson")]
    extern "C" {
        pub fn td_json_client_create() -> TdJsonClient;
        pub fn td_json_client_send(client: TdJsonClient, request: *const c_char);
        pub fn td_json_client_receive(client: TdJsonClient, timeout: c_double) -> *const c_char;
        pub fn td_json_client_execute(
            client: TdJsonClient,
            request: *const c_char,
        ) -> *const c_char;
        pub fn td_json_client_destroy(client: TdJsonClient);
        pub fn td_set_log_verbosity_level(level: c_int);
        pub fn td_set_log_message_callback(
            max_verbosity_level: c_int,
            callback: Option<extern "C" fn(verbosity: c_int, message: *const c_char)>,
        );
    }

    /// The callback TDLib calls for each log line. Scrub, then write.
    pub extern "C" fn log_callback(verbosity: c_int, message: *const c_char) {
        if message.is_null() {
            return;
        }
        // SAFETY: TDLib guarantees a NUL-terminated UTF-8 string for the duration
        // of the callback.
        let raw = unsafe { std::ffi::CStr::from_ptr(message) };
        let line = raw.to_string_lossy();
        let scrubbed = crate::logging::redact_line(&line);
        eprintln!(r#"tdlib verbosity={verbosity} {scrubbed}"#);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn version_compare_is_numeric_not_lexicographic() {
        // The bug a lexicographic compare would cause.
        assert!(check_version("1.10.0").is_ok());
        assert!(check_version("1.8.0").is_ok());
        assert!(check_version("1.7.9").is_err());
        assert!(check_version("2.0.0").is_ok());
    }

    #[test]
    fn version_parse_tolerates_suffix() {
        assert_eq!(parse_version("1.8.29").unwrap(), (1, 8, 29));
        assert_eq!(parse_version("1.8.29-rc1").unwrap(), (1, 8, 29));
        assert_eq!(parse_version("1.8").unwrap(), (1, 8, 0));
        assert!(parse_version("vNext").is_err());
    }

    #[test]
    fn authorization_state_mapping() {
        assert_eq!(
            AuthorizationState::from_type_name("authorizationStateReady"),
            Some(AuthorizationState::Ready)
        );
        assert!(AuthorizationState::Ready.can_serve_sync());
        assert!(!AuthorizationState::WaitPassword.can_serve_sync());
    }

    #[test]
    fn parameters_json_carries_the_key_but_not_in_debug() {
        let params = TdlibParameters {
            database_directory: "/var/lib/teleforge-agent/t/accounts/a".to_string(),
            files_directory: "/var/lib/teleforge-agent/t/accounts/a/files".to_string(),
            use_message_database: true,
            use_secret_chats: true,
            use_chat_info_database: true,
            system_language_code: "en".to_string(),
            device_model: "TeleForge Agent".to_string(),
            system_version: "linux".to_string(),
            application_version: "0.1.0".to_string(),
            api_id: 1,
            api_hash: "hash".to_string(),
            enable_storage_optimizer: true,
        };
        let key = crate::secret::SecretBytes::new(vec![0xABu8; 32]);
        let json = params.to_set_parameters_json(&key);
        assert!(json.contains("\"database_encryption_key\":\"abab"));
        assert!(json.contains("\"@type\":\"setTdlibParameters\""));
        // The Debug of the key must not leak the bytes.
        assert!(!format!("{key:?}").contains("abab"));
    }
}
