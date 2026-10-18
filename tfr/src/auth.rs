//! Client → agent authentication.
//!
//! # Threat model in one paragraph
//!
//! The agent listens on loopback only and is reached through the SSH connection
//! the client used to install it. So the network attacker is already excluded by
//! SSH. What remains is (a) *another local process* on the same VPS, (b) a
//! *different* SSH user who can reach the forwarded port, and (c) a stolen
//! client backup containing the credential. The scheme below is layered so that
//! each of those loses on its own:
//!
//! | Layer | Proves | Material | Where it lives |
//! |---|---|---|---|
//! | SSH channel | who may open the connection at all | the user's own ssh key/password | never touches the agent |
//! | TLS 1.3 (pinned cert) | this is the agent that was installed | self-signed cert, fingerprint pinned by the client | agent: `identity/agent.cert.pem` |
//! | Bearer device token | this device was enrolled by someone with console access | 32 random bytes, shown once | client: OS keychain; agent: `auth/clients.json` with `sha256(token)`, `0600` |
//!
//! # Why a bearer token and not a per-request Ed25519 signature
//!
//! `docs/architecture.md` § 6 offers both. The token was chosen for this step, and
//! the reasons are concrete rather than aesthetic:
//!
//! 1. **The signature has no implementation yet.** `crypto::sig` is a stub
//!    (Ed25519 is scheduled for the `updater` step). A signature scheme implemented
//!    *for* the transport would be the first real use of a primitive in the crate —
//!    the wrong order to introduce one.
//! 2. **Signing does not remove the trust anchor.** Verifying a request signature
//!    requires a registered public key per device. That is the same enrollment step,
//!    the same file, the same revocation problem — the only thing that changes is
//!    that replay protection stops being TLS's job and becomes ours (nonce window,
//!    clock skew, per-connection state). `ReplayWindow` exists for the day it is
//!    needed; it is not needed while TLS 1.3 gives per-record authentication and a
//!    fresh session key for every connection.
//! 3. **The token is bound to the channel it was minted for.** It is only accepted
//!    over the pinned TLS listener, and the listener is only reachable through the
//!    SSH forward. A stolen token file is useless without the client's SSH access to
//!    that server.
//!
//! So: the token authenticates the *request*, TLS authenticates the *endpoint*, and
//! SSH authenticates the *person*. A future step can add request signatures without
//! breaking this, because a `ClientRecord` already has room for a public key.
//!
//! # Why the token is stored as a plain SHA-256 of 32 random bytes
//!
//! Argon2id exists to slow down *guessing*. A 256-bit random token is not
//! guessable, so a slow KDF would only make every request slower. Passwords get
//! Argon2id; tokens get SHA-256; that split is intentional and is written down
//! here so a later reader does not "fix consistency".
//!
//! # Enrollment
//!
//! `teleforge-agent pair --label "MacBook" --tenant <hex> --account <hex>` mints a
//! token, writes only its hash, and prints the token once. The record binds the
//! device to one tenant and one account, which is what makes "push into someone
//! else's account" a 403 rather than a per-event rejection.

use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};

use crate::error::{Error, Result};
use crate::fsperm;
use crate::store::AccountId;

pub const TOKEN_BYTES: usize = 32;
pub const DEVICE_ID_BYTES: usize = 16;
pub const PAIRING_CODE_BYTES: usize = 16;
/// How long a pairing code stays valid. Short, because it travels through a
/// human ("copy this from the client into the VPS console") or through the SSH
/// channel that just installed the agent.
pub const PAIRING_CODE_TTL_SECS: u64 = 600;
/// Replayed/delayed request tolerance.
pub const MAX_CLOCK_SKEW_SECS: u64 = 120;
/// Relative to the data directory.
pub const CLIENTS_FILE: &str = "auth/clients.json";
/// Bumped when the file layout changes incompatibly. Unknown versions are refused
/// rather than parsed leniently: this file is the only thing between a stranger and
/// the sync store.
pub const CLIENTS_FORMAT: u32 = 1;

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub enum Scope {
    Status,
    SyncRead,
    SyncWrite,
    Tunnel,
    Unlock,
    Update,
    /// Run one server-side DB ingest pass (`db.ingest`).
    ///
    /// Never granted by `pair` (§ 15 п. 11 of `docs/transport.md`): the pull-first
    /// model makes the ingest an *internal* activity, so only a token explicitly
    /// enrolled with [`TokenRole::Ingest`] can trigger it. The grant and the role are
    /// two independent checks, on purpose — `enroll_with_role` refuses to mint a
    /// reader token carrying this scope, and [`ClientRecord::may_ingest`] checks the
    /// role again at request time.
    DbIngest,
    /// Only minted for the client that performed the install; allows revoking
    /// other devices and rotating the listener certificate.
    Admin,
}

impl Scope {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Status => "status",
            Self::SyncRead => "sync:read",
            Self::SyncWrite => "sync:write",
            Self::Tunnel => "tunnel",
            Self::Unlock => "unlock",
            Self::Update => "update",
            Self::DbIngest => "db:ingest",
            Self::Admin => "admin",
        }
    }

    pub fn parse(value: &str) -> Option<Self> {
        match value {
            "status" => Some(Self::Status),
            "sync:read" => Some(Self::SyncRead),
            "sync:write" => Some(Self::SyncWrite),
            "tunnel" => Some(Self::Tunnel),
            "unlock" => Some(Self::Unlock),
            "update" => Some(Self::Update),
            "db:ingest" => Some(Self::DbIngest),
            "admin" => Some(Self::Admin),
            _ => None,
        }
    }
}

/// What kind of credential a token is.
///
/// The client's token is a **reader** (`docs/transport.md` § 2.2: "Токен — это
/// читатель"): `status`, `sync.stamp`/`pull`/`conflicts`, local `sync.push`, and
/// `db.status`. The *server-side* ingest never travels over HTTP with a client token;
/// when it has to be triggered by hand it needs a token whose role says so, and
/// `pair` cannot mint one by accident.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum TokenRole {
    /// Everything the desktop client does. The default, and the only role `pair`
    /// mints unless asked otherwise.
    #[default]
    Reader,
    /// Internal/operator token: the reader scopes plus `db:ingest`.
    Ingest,
}

impl TokenRole {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Reader => "reader",
            Self::Ingest => "ingest",
        }
    }

    pub fn parse(value: &str) -> Option<Self> {
        match value.trim().to_ascii_lowercase().as_str() {
            "reader" | "read" => Some(Self::Reader),
            "ingest" | "internal" => Some(Self::Ingest),
            _ => None,
        }
    }
}

/// What a device may do, and for which account.
#[derive(Clone, Debug)]
pub struct ClientRecord {
    pub device_id: [u8; DEVICE_ID_BYTES],
    pub label: String,
    /// `sha256(token)`; the token itself is never stored.
    pub token_hash: [u8; 32],
    pub scopes: Vec<Scope>,
    /// `reader` or `ingest` (§ 15 п. 11). Part of the record, so "what may this token
    /// do" is answerable from the persisted file and not only from its scope list.
    pub role: TokenRole,
    /// The only tenant this device may address. `None` means "any", which is what
    /// the installer's own device gets before the first tenant exists.
    pub tenant: Option<String>,
    /// The only account this device may address.
    pub account_id: Option<AccountId>,
    pub created_at: u64,
    pub last_seen_at: Option<u64>,
    pub revoked: bool,
}

impl ClientRecord {
    pub fn allows(&self, scope: Scope) -> bool {
        if self.revoked {
            return false;
        }
        // Two independent conditions for the ingest scope: the grant and the role.
        // Either one alone is a bug waiting to be a privilege escalation.
        if scope == Scope::DbIngest && self.role != TokenRole::Ingest {
            return false;
        }
        self.scopes.contains(&Scope::Admin) || self.scopes.contains(&scope)
    }

    /// Whether this credential may trigger the server-side ingest.
    pub fn may_ingest(&self) -> bool {
        self.allows(Scope::DbIngest)
    }

    pub fn is_reader(&self) -> bool {
        self.role == TokenRole::Reader
    }

    pub fn device_id_hex(&self) -> String {
        crate::crypto::sig::to_hex(&self.device_id)
    }

    /// Whether this device may act on `(tenant, account)`.
    ///
    /// A record with `tenant: None` / `account_id: None` is unrestricted; that is
    /// only produced by `pair` without those flags, and the client should always
    /// pass them. Comparing is exact (hex string, 16 bytes): the goal is to make a
    /// mixed-up account id a 403, not to be clever about prefixes.
    pub fn owns(&self, tenant: Option<&str>, account: Option<&AccountId>) -> bool {
        self.allows_tenant(tenant)
            && match (&self.account_id, account) {
                (Some(allowed), Some(requested)) => allowed == requested,
                (Some(_), None) => false,
                (None, _) => true,
            }
    }

    /// Tenant half of [`Self::owns`], used where the account is resolved
    /// separately (the API layer checks the account against the request first, so
    /// re-checking it here would only re-compare a value with itself).
    pub fn allows_tenant(&self, tenant: Option<&str>) -> bool {
        match (&self.tenant, tenant) {
            (Some(allowed), Some(requested)) => allowed.eq_ignore_ascii_case(requested),
            (Some(_), None) => false,
            (None, _) => true,
        }
    }
}

/// The persisted set of devices.
///
/// `Debug` is derived on purpose: `token_hash` is a hash, not a token, and seeing
/// which device failed to authenticate is worth more than hiding a SHA-256.
#[derive(Debug)]
pub struct AuthStore {
    path: PathBuf,
    clients: Vec<ClientRecord>,
}

impl AuthStore {
    /// Open `<data_dir>/auth/clients.json`, or an empty store when the file does not
    /// exist yet (a freshly installed agent before `pair`).
    pub fn load(data_dir: &str) -> Result<Self> {
        let path = Path::new(data_dir).join(CLIENTS_FILE);
        Self::load_path(path)
    }

    pub fn load_path(path: impl Into<PathBuf>) -> Result<Self> {
        let path = path.into();
        if !path.exists() {
            return Ok(Self {
                path,
                clients: Vec::new(),
            });
        }
        let text = std::fs::read_to_string(&path).map_err(|error| {
            Error::Io(std::io::Error::new(
                error.kind(),
                format!("cannot read {}: {error}", path.display()),
            ))
        })?;
        let file: ClientsFile = serde_json::from_str(&text).map_err(|error| {
            Error::Invalid(format!(
                "{} is not valid clients JSON: {error}",
                path.display()
            ))
        })?;
        if file.version != CLIENTS_FORMAT {
            return Err(Error::Invalid(format!(
                "{} is clients format {}, this build writes {CLIENTS_FORMAT}",
                path.display(),
                file.version
            )));
        }
        let mut clients = Vec::with_capacity(file.clients.len());
        for entry in file.clients {
            clients.push(entry.into_record()?);
        }
        Ok(Self { path, clients })
    }

    pub fn path(&self) -> &Path {
        &self.path
    }

    pub fn is_empty(&self) -> bool {
        self.clients.is_empty()
    }

    pub fn len(&self) -> usize {
        self.clients.len()
    }

    pub fn records(&self) -> &[ClientRecord] {
        &self.clients
    }

    /// Mint a token for a new device and persist its hash.
    ///
    /// Returns the token exactly once: it is the caller's job to show it (the CLI
    /// prints it) and there is no read-back path.
    pub fn enroll(
        &mut self,
        label: &str,
        scopes: Vec<Scope>,
        tenant: Option<String>,
        account_id: Option<AccountId>,
        now: u64,
    ) -> Result<(crate::secret::SecretBytes, ClientRecord)> {
        self.enroll_with_role(label, TokenRole::Reader, scopes, tenant, account_id, now)
    }

    /// [`Self::enroll`] with an explicit role.
    ///
    /// Enforces the invariant of § 15 п. 11: a `reader` token may not hold
    /// [`Scope::DbIngest`]. Without this, "the client token cannot start the ingest"
    /// would depend on every future caller passing the right scope list.
    pub fn enroll_with_role(
        &mut self,
        label: &str,
        role: TokenRole,
        scopes: Vec<Scope>,
        tenant: Option<String>,
        account_id: Option<AccountId>,
        now: u64,
    ) -> Result<(crate::secret::SecretBytes, ClientRecord)> {
        if label.len() > 64 {
            return Err(Error::Invalid("label must be at most 64 bytes".to_string()));
        }
        if role == TokenRole::Reader && scopes.contains(&Scope::DbIngest) {
            return Err(Error::Invalid(
                "a reader token cannot hold db:ingest; enroll it with role=ingest".to_string(),
            ));
        }
        let mut device_id = [0u8; DEVICE_ID_BYTES];
        fill_random(&mut device_id)?;
        let token = crate::secret::SecretBytes::generate(TOKEN_BYTES)?;
        let token_hash = token_hash(&token)?;
        let record = ClientRecord {
            device_id,
            label: label.to_string(),
            token_hash,
            scopes,
            role,
            tenant: tenant.map(|value| value.to_ascii_lowercase()),
            account_id,
            created_at: now,
            last_seen_at: None,
            revoked: false,
        };
        self.clients.push(record.clone());
        self.save()?;
        Ok((token, record))
    }

    /// Constant-time token check over every record.
    ///
    /// Iterating all records rather than stopping at the first match keeps the
    /// timing independent of *which* device matched, and `constant_time_eq` keeps it
    /// independent of how many bytes matched.
    pub fn authenticate(
        &mut self,
        token: &crate::secret::SecretBytes,
        now: u64,
    ) -> Result<ClientRecord> {
        let presented = token_hash(token)?;
        let mut matched: Option<usize> = None;
        for (index, client) in self.clients.iter().enumerate() {
            if crate::secret::constant_time_eq(&client.token_hash, &presented) {
                matched = Some(index);
            }
        }
        let index = matched.ok_or_else(|| Error::Auth("unknown device token".to_string()))?;
        if self.clients[index].revoked {
            return Err(Error::Auth("device token has been revoked".to_string()));
        }
        // Deliberately not persisted: `last_seen_at` would turn every read into a
        // write, and losing it on restart costs nothing.
        self.clients[index].last_seen_at = Some(now);
        Ok(self.clients[index].clone())
    }

    pub fn revoke(&mut self, device_id: &str) -> Result<()> {
        let wanted = crate::crypto::sig::parse_hex(device_id)?;
        let mut found = false;
        for client in self.clients.iter_mut() {
            if client.device_id.to_vec() == wanted {
                client.revoked = true;
                found = true;
            }
        }
        if !found {
            return Err(Error::Invalid(format!("no device {device_id:?}")));
        }
        self.save()
    }

    /// Write the file `0600`, atomically.
    pub fn save(&self) -> Result<()> {
        let file = ClientsFile {
            version: CLIENTS_FORMAT,
            clients: self.clients.iter().map(ClientEntry::from_record).collect(),
        };
        let text = serde_json::to_string_pretty(&file)
            .map_err(|error| Error::Invalid(format!("cannot serialise clients: {error}")))?;
        if let Some(parent) = self.path.parent() {
            fsperm::create_private_dir(&parent.to_string_lossy())?;
        }
        fsperm::write_private_file_replace(&self.path.to_string_lossy(), text.as_bytes())
    }

    /// Human-readable summary for `status`; contains no token material.
    pub fn status_line(&self) -> String {
        let active = self.clients.iter().filter(|client| !client.revoked).count();
        crate::logging::redact_line(&format!(
            "clients={active} revoked={} ",
            self.clients.len() - active
        ))
    }
}

/// `sha256` of the token. Not a KDF — see the module docs.
pub fn token_hash(token: &crate::secret::SecretBytes) -> Result<[u8; 32]> {
    if token.len() != TOKEN_BYTES {
        return Err(Error::Auth(format!(
            "token must be {TOKEN_BYTES} bytes, got {}",
            token.len()
        )));
    }
    use sha2::{Digest, Sha256};
    let digest = Sha256::digest(token.expose());
    let mut out = [0u8; 32];
    out.copy_from_slice(&digest);
    Ok(out)
}

fn fill_random(buf: &mut [u8]) -> Result<()> {
    use rand::RngCore;
    rand::rngs::OsRng.fill_bytes(buf);
    Ok(())
}

#[derive(Debug, Serialize, Deserialize)]
struct ClientsFile {
    version: u32,
    clients: Vec<ClientEntry>,
}

#[derive(Debug, Serialize, Deserialize)]
struct ClientEntry {
    device_id: String,
    label: String,
    token_hash: String,
    #[serde(default)]
    scopes: Vec<String>,
    /// `reader`/`ingest`. Absent in a file written before roles existed, which is why
    /// it defaults to the reader role: an old token keeps exactly the rights it had.
    #[serde(default)]
    role: Option<String>,
    #[serde(default)]
    tenant: Option<String>,
    #[serde(default)]
    account_id: Option<String>,
    #[serde(default)]
    created_at: u64,
    #[serde(default)]
    last_seen_at: Option<u64>,
    #[serde(default)]
    revoked: bool,
}

impl ClientEntry {
    fn from_record(record: &ClientRecord) -> Self {
        Self {
            device_id: record.device_id_hex(),
            label: record.label.clone(),
            token_hash: crate::crypto::sig::to_hex(&record.token_hash),
            scopes: record
                .scopes
                .iter()
                .map(|scope| scope.as_str().to_string())
                .collect(),
            role: Some(record.role.as_str().to_string()),
            tenant: record.tenant.clone(),
            account_id: record.account_id.map(|account| account.to_hex()),
            created_at: record.created_at,
            last_seen_at: record.last_seen_at,
            revoked: record.revoked,
        }
    }

    fn into_record(self) -> Result<ClientRecord> {
        let device_id = crate::crypto::sig::parse_hex(&self.device_id)?;
        if device_id.len() != DEVICE_ID_BYTES {
            return Err(Error::Invalid(format!(
                "device_id must be {DEVICE_ID_BYTES} bytes, got {}",
                device_id.len()
            )));
        }
        let mut bytes = [0u8; DEVICE_ID_BYTES];
        bytes.copy_from_slice(&device_id);
        let hash = crate::crypto::sig::parse_hex(&self.token_hash)?;
        if hash.len() != 32 {
            return Err(Error::Invalid("token_hash must be 32 bytes".to_string()));
        }
        let mut token_hash = [0u8; 32];
        token_hash.copy_from_slice(&hash);
        let mut scopes = Vec::with_capacity(self.scopes.len());
        for scope in &self.scopes {
            let parsed = Scope::parse(scope).ok_or_else(|| {
                Error::Invalid(format!("unknown scope {scope:?} in {CLIENTS_FILE}"))
            })?;
            if !scopes.contains(&parsed) {
                scopes.push(parsed);
            }
        }
        let role = match &self.role {
            Some(text) => TokenRole::parse(text).ok_or_else(|| {
                Error::Invalid(format!("unknown role {text:?} in {CLIENTS_FILE}"))
            })?,
            None => TokenRole::Reader,
        };
        // A hand-edited file must not be able to grant the ingest scope to a reader.
        if role == TokenRole::Reader && scopes.contains(&Scope::DbIngest) {
            return Err(Error::Invalid(format!(
                "a reader token in {CLIENTS_FILE} cannot hold db:ingest"
            )));
        }
        let account_id = match &self.account_id {
            Some(hex) => Some(AccountId::from_hex(hex)?),
            None => None,
        };
        Ok(ClientRecord {
            device_id: bytes,
            label: self.label,
            token_hash,
            scopes,
            role,
            tenant: self.tenant,
            account_id,
            created_at: self.created_at,
            last_seen_at: self.last_seen_at,
            revoked: self.revoked,
        })
    }
}

/// Simple per-device rate limiter, to keep a compromised token from hammering
/// `unlock` (which is Argon2id, i.e. a memory amplifier) or from turning the store
/// into a write amplifier.
#[derive(Debug)]
pub struct RateLimiter {
    pub capacity: u32,
    pub refill_per_sec: u32,
    pub tokens: f64,
    pub last_refill_ms: u64,
}

impl RateLimiter {
    pub fn new(capacity: u32, refill_per_sec: u32, now_ms: u64) -> Self {
        Self {
            capacity: capacity.max(1),
            refill_per_sec,
            tokens: f64::from(capacity.max(1)),
            last_refill_ms: now_ms,
        }
    }

    pub fn try_take(&mut self, now_ms: u64) -> bool {
        if now_ms > self.last_refill_ms {
            let elapsed = (now_ms - self.last_refill_ms) as f64 / 1000.0;
            self.tokens = (self.tokens + elapsed * f64::from(self.refill_per_sec))
                .min(f64::from(self.capacity));
            self.last_refill_ms = now_ms;
        }
        if self.tokens >= 1.0 {
            self.tokens -= 1.0;
            true
        } else {
            false
        }
    }
}

/// Replay window for request nonces. Per-connection, in memory: a restart forgets
/// old nonces, which is fine because the session itself is gone too.
///
/// Unused while the bearer token over TLS is the authentication mechanism (see the
/// module docs); it is kept because the alternative — a signed request — needs the
/// same thing, and a tested implementation is cheaper than remembering to write one.
#[derive(Debug, Default)]
pub struct ReplayWindow {
    pub last_counter: u64,
    pub seen: Vec<u64>,
}

impl ReplayWindow {
    pub const MAX_ENTRIES: usize = 1024;

    pub fn accept(&mut self, counter: u64) -> bool {
        if self.seen.contains(&counter) {
            return false;
        }
        if counter + Self::MAX_ENTRIES as u64 <= self.last_counter {
            // Too old to prove anything; drop silently rather than grow `seen`.
            return false;
        }
        self.seen.push(counter);
        if self.seen.len() > Self::MAX_ENTRIES {
            self.seen.remove(0);
        }
        if counter > self.last_counter {
            self.last_counter = counter;
        }
        true
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::test_support::TestDir;

    fn store_in(directory: &TestDir) -> AuthStore {
        AuthStore::load_path(directory.join(CLIENTS_FILE)).unwrap()
    }

    fn token(bytes: u8) -> crate::secret::SecretBytes {
        crate::secret::SecretBytes::new(vec![bytes; TOKEN_BYTES])
    }

    fn account() -> AccountId {
        AccountId::new([0x11; 16]).unwrap()
    }

    #[test]
    fn enrollment_persists_only_the_hash() {
        let directory = TestDir::new("auth");
        let mut store = store_in(&directory);
        assert!(store.is_empty());
        let (secret, record) = store
            .enroll(
                "MacBook",
                vec![Scope::SyncRead, Scope::SyncWrite],
                Some("00112233445566778899aabbccddeeff".to_string()),
                Some(account()),
                1_000,
            )
            .unwrap();
        assert_eq!(record.device_id.len(), 16);
        assert_eq!(record.label, "MacBook");

        let text = std::fs::read_to_string(store.path()).unwrap();
        let token_hex = crate::crypto::sig::to_hex(secret.expose());
        assert!(
            !text.contains(&token_hex),
            "the token itself must never be written"
        );
        assert!(text.contains(&crate::crypto::sig::to_hex(&record.token_hash)));
        assert!(text.contains("sync:read"));

        // Reloading the file yields the same record and still authenticates.
        let mut reloaded = AuthStore::load_path(store.path()).unwrap();
        assert_eq!(reloaded.len(), 1);
        let authenticated = reloaded.authenticate(&secret, 2_000).unwrap();
        assert_eq!(authenticated.device_id_hex(), record.device_id_hex());
        assert_eq!(authenticated.last_seen_at, Some(2_000));
    }

    #[test]
    fn a_wrong_token_is_refused_and_the_check_touches_every_record() {
        let directory = TestDir::new("auth");
        let mut store = store_in(&directory);
        let (good, _) = store
            .enroll("a", vec![Scope::SyncRead], None, None, 1)
            .unwrap();
        store
            .enroll("b", vec![Scope::SyncWrite], None, None, 2)
            .unwrap();
        let error = store.authenticate(&token(0xff), 3).unwrap_err();
        assert!(matches!(error, Error::Auth(_)));
        assert!(store.authenticate(&good, 4).is_ok());
    }

    #[test]
    fn a_revoked_device_stops_authenticating() {
        let directory = TestDir::new("auth");
        let mut store = store_in(&directory);
        let (secret, record) = store
            .enroll("a", vec![Scope::SyncRead], None, None, 1)
            .unwrap();
        store.revoke(&record.device_id_hex()).unwrap();
        let error = store.authenticate(&secret, 2).unwrap_err();
        assert!(error.to_string().contains("revoked"), "{error}");
        assert!(store.revoke("00112233").is_err());
    }

    #[test]
    fn a_record_is_bound_to_its_tenant_and_account() {
        let directory = TestDir::new("auth");
        let mut store = store_in(&directory);
        let (secret, _) = store
            .enroll(
                "a",
                vec![Scope::SyncWrite],
                Some("aabb".to_string()),
                Some(account()),
                1,
            )
            .unwrap();
        let record = store.authenticate(&secret, 2).unwrap();
        assert!(record.owns(Some("aabb"), Some(&account())));
        assert!(!record.owns(Some("ccdd"), Some(&account())), "other tenant");
        let other = AccountId::new([0x22; 16]).unwrap();
        assert!(!record.owns(Some("aabb"), Some(&other)), "other account");
        // Omission is a refusal, not a wildcard: the client must say which account.
        assert!(!record.owns(None, None));
    }

    #[test]
    fn scopes_gate_the_operations() {
        let directory = TestDir::new("auth");
        let mut store = store_in(&directory);
        let (read_only, _) = store
            .enroll("reader", vec![Scope::SyncRead], None, None, 1)
            .unwrap();
        let (admin, _) = store
            .enroll("installer", vec![Scope::Admin], None, None, 2)
            .unwrap();
        let read_only = store.authenticate(&read_only, 3).unwrap();
        assert!(read_only.allows(Scope::SyncRead));
        assert!(!read_only.allows(Scope::SyncWrite));
        let admin = store.authenticate(&admin, 4).unwrap();
        assert!(admin.allows(Scope::SyncWrite), "admin implies every scope");
        assert!(admin.allows(Scope::Admin));
    }

    #[test]
    fn a_reader_token_cannot_hold_the_ingest_scope() {
        // § 15 п. 11, enforced at enrollment: `pair` mints readers, and a reader that
        // could carry `db:ingest` would make the role field decorative.
        let directory = TestDir::new("auth");
        let mut store = store_in(&directory);
        let error = store
            .enroll(
                "client",
                vec![Scope::SyncRead, Scope::DbIngest],
                None,
                None,
                1,
            )
            .expect_err("a reader must not be granted db:ingest");
        assert!(error.to_string().contains("db:ingest"), "{error}");
        assert!(store.is_empty(), "the refused enrollment must not persist");

        // The ingest role can, and is the only way to get it.
        let (token, record) = store
            .enroll_with_role(
                "ingest",
                TokenRole::Ingest,
                vec![Scope::SyncRead, Scope::DbIngest],
                None,
                None,
                2,
            )
            .unwrap();
        assert!(record.may_ingest());
        assert!(!record.is_reader());
        let authenticated = store.authenticate(&token, 3).unwrap();
        assert!(authenticated.allows(Scope::DbIngest));
    }

    #[test]
    fn the_role_defaults_to_reader_for_a_file_written_before_roles_existed() {
        let directory = TestDir::new("auth");
        let path = directory.join("clients.json");
        std::fs::create_dir_all(directory.path()).unwrap();
        // No `role` key at all: an older agent's file.
        let legacy = format!(
            r#"{{"version":1,"clients":[{{"device_id":"{}","label":"x",
                "token_hash":"{}","scopes":["sync:read"]}}]}}"#,
            "11".repeat(16),
            "22".repeat(32)
        );
        std::fs::write(&path, legacy).unwrap();
        let store = AuthStore::load_path(&path).unwrap();
        assert_eq!(store.len(), 1);
        assert!(store.records()[0].is_reader());
        assert!(!store.records()[0].may_ingest());
    }

    #[test]
    fn a_hand_edited_file_cannot_promote_a_reader_to_ingest() {
        let directory = TestDir::new("auth");
        let path = directory.join("clients.json");
        std::fs::create_dir_all(directory.path()).unwrap();
        let smuggled = format!(
            r#"{{"version":1,"clients":[{{"device_id":"{}","label":"x",
                "token_hash":"{}","scopes":["sync:read","db:ingest"],"role":"reader"}}]}}"#,
            "11".repeat(16),
            "22".repeat(32)
        );
        std::fs::write(&path, smuggled).unwrap();
        let error = AuthStore::load_path(&path).expect_err("the file must be refused");
        assert!(error.to_string().contains("db:ingest"), "{error}");

        let unknown_role = format!(
            r#"{{"version":1,"clients":[{{"device_id":"{}","label":"x",
                "token_hash":"{}","scopes":[],"role":"root"}}]}}"#,
            "11".repeat(16),
            "22".repeat(32)
        );
        std::fs::write(&path, unknown_role).unwrap();
        assert!(AuthStore::load_path(&path).is_err());
    }

    #[test]
    fn the_role_survives_a_save_and_reload() {
        let directory = TestDir::new("auth");
        let mut store = store_in(&directory);
        store
            .enroll_with_role(
                "ingest",
                TokenRole::Ingest,
                vec![Scope::DbIngest],
                None,
                None,
                1,
            )
            .unwrap();
        store
            .enroll("client", vec![Scope::SyncRead], None, None, 2)
            .unwrap();
        let reloaded = AuthStore::load_path(store.path()).unwrap();
        let roles: Vec<TokenRole> = reloaded.records().iter().map(|c| c.role).collect();
        assert_eq!(roles, vec![TokenRole::Ingest, TokenRole::Reader]);
        // The text on disk is explicit, so an operator can audit it.
        let text = std::fs::read_to_string(store.path()).unwrap();
        assert!(text.contains("\"role\": \"ingest\""), "{text}");
    }

    #[test]
    fn a_corrupt_or_future_file_is_refused() {
        let directory = TestDir::new("auth");
        let path = directory.join("clients.json");
        std::fs::create_dir_all(directory.path()).unwrap();
        std::fs::write(&path, b"not json").unwrap();
        assert!(AuthStore::load_path(&path).is_err());

        std::fs::write(&path, br#"{"version":9,"clients":[]}"#).unwrap();
        let error = AuthStore::load_path(&path).unwrap_err();
        assert!(error.to_string().contains("format 9"), "{error}");
    }

    #[test]
    fn a_record_with_a_bad_scope_or_short_hash_is_refused() {
        let directory = TestDir::new("auth");
        let path = directory.join("clients.json");
        std::fs::create_dir_all(directory.path()).unwrap();
        let bad_scope = format!(
            r#"{{"version":1,"clients":[{{"device_id":"{}","label":"x",
                "token_hash":"{}","scopes":["frobnicate"]}}]}}"#,
            "11".repeat(16),
            "22".repeat(32)
        );
        std::fs::write(&path, bad_scope).unwrap();
        assert!(AuthStore::load_path(&path).is_err());

        let short_hash = format!(
            r#"{{"version":1,"clients":[{{"device_id":"{}","label":"x",
                "token_hash":"22","scopes":[]}}]}}"#,
            "11".repeat(16)
        );
        std::fs::write(&path, short_hash).unwrap();
        assert!(AuthStore::load_path(&path).is_err());
    }

    #[test]
    fn debug_output_never_contains_the_token() {
        let directory = TestDir::new("auth");
        let mut store = store_in(&directory);
        let (secret, _) = store
            .enroll("a", vec![Scope::SyncRead], None, None, 1)
            .unwrap();
        let rendered = format!("{store:?}");
        let token_hex = crate::crypto::sig::to_hex(secret.expose());
        assert!(!rendered.contains(&token_hex));
        assert!(rendered.contains("token_hash"));
    }

    #[test]
    fn the_rate_limiter_refills_and_caps() {
        let mut limiter = RateLimiter::new(2, 1, 0);
        assert!(limiter.try_take(0));
        assert!(limiter.try_take(0));
        assert!(!limiter.try_take(0), "empty");
        // One second later one token is back.
        assert!(limiter.try_take(1_000));
        assert!(!limiter.try_take(1_000));
        // Capacity is a ceiling, not a running total.
        assert!(limiter.try_take(10_000));
        assert!(limiter.try_take(10_000));
        assert!(!limiter.try_take(10_000));
    }

    #[test]
    fn the_replay_window_keeps_a_bounded_sliding_window() {
        let mut window = ReplayWindow::default();
        assert!(window.accept(1));
        assert!(!window.accept(1), "duplicate");
        assert!(window.accept(2));
        for counter in 3..(ReplayWindow::MAX_ENTRIES as u64 + 10) {
            assert!(window.accept(counter));
        }
        assert!(window.seen.len() <= ReplayWindow::MAX_ENTRIES);
        // Far in the past: refused without growing the window.
        assert!(!window.accept(1));
    }
}
