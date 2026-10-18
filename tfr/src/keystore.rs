//! Per-account secret envelope. This is the answer to audit finding **C1**.
//!
//! # What C1 was
//!
//! `teleforge_sync.cpp` derived the cloud encryption key from
//! `QByteArray::number(user->id) + user->phone()` through a single `SHA-256` that
//! the code called `HkdfSha256`. Both inputs are public and enumerable, so anyone
//! holding a sync-channel dump or a database dump could derive the key offline.
//! There was no salt, no work factor, and no secret anywhere in the chain.
//!
//! # What replaces it
//!
//! ```text
//! salt   = 32 random bytes, stored next to the envelope
//! KEK    = Argon2id(passphrase, salt, m_cost, t_cost, p_cost)        // 32 bytes
//! DEK    = 32 random bytes, generated once per account
//! wrap   = XChaCha20-Poly1305(KEK, aad = account_id || key_id || "dek", DEK)
//! check  = XChaCha20-Poly1305(KEK, aad = account_id || key_id || "verifier",
//!                             CONSTANT)
//! ```
//!
//! Three properties matter and each one has a test:
//!
//! 1. **The key contains a real secret.** The passphrase is never derived from an
//!    account id, a phone number or anything else the server can see.
//! 2. **Two accounts with the same passphrase get different keys.** The salt is
//!    random per account, so there is nothing to precompute.
//! 3. **The verifier is not a hash of the KEK.** It is a fixed plaintext sealed
//!    under the KEK, so a wrong passphrase fails an AEAD tag instead of matching a
//!    digest. A stored `SHA-256(KEK)` would hand an offline attacker a one-step
//!    oracle on top of Argon2id.
//!
//! The AAD is `account_id || key_id` exactly as the design requires, with one
//! extra trailing purpose byte (`b'd'` for the wrapped DEK, `b'v'` for the
//! verifier). Without that byte the two blobs would be interchangeable: an
//! attacker with write access to the directory could move the verifier blob into
//! the `wrapped_dek` slot and have the agent treat 34 known bytes as the DEK.
//!
//! # Trait split
//!
//! [`Kdf`] and [`Aead`] exist so the envelope logic can be tested without paying
//! Argon2id's memory-and-time cost on every case, and so a future backend (HSM,
//! `mlock`ed key store) can be swapped in. There are two implementations of each:
//!
//! * [`Argon2Kdf`] / [`PrimitiveAead`] — the real primitives.
//! * [`StubKdf`] / [`StubAead`] — deterministic, cheap, and **refused in release
//!   builds**: they return an error instead of silently protecting nothing.
//!
//! # Files and permissions
//!
//! `<dir>/accounts/<account_hex>.envelope.json`, mode `0600`, written through
//! [`crate::fsperm::write_private_file_replace`] (temp file + `rename` + `fsync`
//! of the directory). The passphrase and the DEK never appear in the file, in
//! `Debug`, or in any log line — the `Debug` impls below print lengths only.

use std::path::{Path, PathBuf};

use sha2::{Digest, Sha256};

use crate::crypto::aead::{self, AeadAlgorithm, Header};
use crate::crypto::kdf::{self, KdfCost, KEY_LEN, SALT_LEN};
use crate::error::{Error, Result};
use crate::fsperm;
use crate::secret::{constant_time_eq, SecretBytes};
use crate::store::event::AccountId;
use crate::sync::clock::Clock;

pub const FORMAT_VERSION: u32 = 1;

/// The fixed plaintext the verifier is an AEAD of. Not a key, not a hash of a key.
pub const VERIFIER_PLAINTEXT: &[u8] = b"teleforge-keystore-verifier-v1";

/// AAD purpose tags; see the module docs for why they exist.
pub const PURPOSE_DEK: u8 = b'd';
pub const PURPOSE_VERIFIER: u8 = b'v';
/// Second wrap of the same DEK, under a KEK derived from the recovery code.
///
/// The purpose byte is what keeps the two wraps non-interchangeable: a recovery
/// wrap moved into the `wrapped_dek` slot fails the tag instead of unlocking the
/// account with the recovery code, and vice versa.
pub const PURPOSE_RECOVERY: u8 = b'r';
/// Recovery code length. 16 random bytes = 128 bits, the same as the tenant-level
/// `vault` code, and long enough that guessing is not the cheapest attack.
pub const RECOVERY_CODE_LEN: usize = 16;

/// Password-based key derivation.
pub trait Kdf {
    /// Numeric id stored in the envelope.
    fn id(&self) -> u16;

    /// Short name for logs and `self-test`.
    fn name(&self) -> &'static str;

    /// Derive a 32-byte KEK. `salt` is always [`SALT_LEN`] bytes.
    fn derive(&self, passphrase: &SecretBytes, salt: &[u8], cost: KdfCost) -> Result<SecretBytes>;
}

/// Authenticated encryption with associated data.
pub trait Aead {
    fn id(&self) -> u16;

    fn name(&self) -> &'static str;

    fn seal(&self, key: &SecretBytes, key_id: u16, aad: &[u8], plain: &[u8]) -> Result<Vec<u8>>;

    fn open(&self, key: &SecretBytes, aad: &[u8], sealed: &[u8]) -> Result<SecretBytes>;
}

/// Argon2id v1.3 — the real KDF.
#[derive(Clone, Copy, Debug, Default)]
pub struct Argon2Kdf;

impl Kdf for Argon2Kdf {
    fn id(&self) -> u16 {
        1
    }

    fn name(&self) -> &'static str {
        "argon2id"
    }

    fn derive(&self, passphrase: &SecretBytes, salt: &[u8], cost: KdfCost) -> Result<SecretBytes> {
        kdf::derive_kek_with(passphrase, salt, cost)
    }
}

/// XChaCha20-Poly1305 — the real AEAD used to wrap the DEK.
#[derive(Clone, Copy, Debug)]
pub struct PrimitiveAead {
    algorithm: AeadAlgorithm,
}

impl Default for PrimitiveAead {
    fn default() -> Self {
        Self {
            algorithm: AeadAlgorithm::XChaCha20Poly1305,
        }
    }
}

impl PrimitiveAead {
    pub fn new() -> Self {
        Self::default()
    }

    /// AES-256-GCM is accepted for reading legacy `TFSYNC1` blobs, but the
    /// writer should not choose it: its 96-bit nonce needs a counter.
    pub fn with_algorithm(algorithm: AeadAlgorithm) -> Self {
        Self { algorithm }
    }

    pub fn algorithm(&self) -> AeadAlgorithm {
        self.algorithm
    }
}

impl Aead for PrimitiveAead {
    fn id(&self) -> u16 {
        self.algorithm.id()
    }

    fn name(&self) -> &'static str {
        "xchacha20-poly1305"
    }

    fn seal(&self, key: &SecretBytes, key_id: u16, aad: &[u8], plain: &[u8]) -> Result<Vec<u8>> {
        aead::seal(key, key_id, self.algorithm, aad, plain)
    }

    fn open(&self, key: &SecretBytes, aad: &[u8], sealed: &[u8]) -> Result<SecretBytes> {
        aead::open(key, aad, sealed)
    }
}

/// Deterministic stand-in for a KDF. **Not a KDF.**
///
/// It is a labelled `SHA-256` chain: cheap, deterministic per
/// `(passphrase, salt, cost)`, and fine for exercising the envelope logic in unit
/// tests. Every other property of a real KDF (work factor, memory hardness) is
/// absent, which is why:
///
/// * [`Self::forbidden_in_release`] makes it return an error in a non-debug build;
/// * `derive` carries `debug_assert!`s so a misuse is loud in tests too.
#[derive(Clone, Copy, Debug, Default)]
pub struct StubKdf;

impl StubKdf {
    /// `true` in any build without `debug_assertions` (i.e. a shipped binary).
    pub const fn forbidden_in_release() -> bool {
        !cfg!(debug_assertions)
    }
}

impl Kdf for StubKdf {
    fn id(&self) -> u16 {
        0xeff0
    }

    fn name(&self) -> &'static str {
        "stub-kdf (test only)"
    }

    fn derive(&self, passphrase: &SecretBytes, salt: &[u8], cost: KdfCost) -> Result<SecretBytes> {
        debug_assert!(
            !Self::forbidden_in_release(),
            "StubKdf must never be used from a release build"
        );
        debug_assert_eq!(salt.len(), SALT_LEN, "StubKdf needs a full-length salt");
        debug_assert!(cost.m_cost_kib > 0, "StubKdf still needs a cost to be told");
        if Self::forbidden_in_release() {
            return Err(Error::Unsupported(
                "StubKdf is a test double and is disabled in release builds",
            ));
        }
        let mut out = Vec::with_capacity(KEY_LEN);
        let mut counter = 0u32;
        while out.len() < KEY_LEN {
            let mut hasher = Sha256::new();
            hasher.update(b"tf/keystore/stub-kdf/v1\0");
            hasher.update(salt);
            hasher.update(passphrase.expose());
            hasher.update(cost.m_cost_kib.to_be_bytes());
            hasher.update(cost.t_cost.to_be_bytes());
            hasher.update(cost.p_cost.to_be_bytes());
            hasher.update(counter.to_be_bytes());
            out.extend_from_slice(&hasher.finalize());
            counter += 1;
        }
        out.truncate(KEY_LEN);
        Ok(SecretBytes::new(out))
    }
}

/// Deterministic stand-in for an AEAD. **Not an AEAD.**
///
/// It keeps the real envelope *header* (so the framing tests are shared) but
/// replaces the cipher with an XOR keystream and the Poly1305 tag with a truncated
/// `SHA-256`. A real [`PrimitiveAead`] cannot open its output and vice versa, which
/// is the correct fail-closed behaviour for a test double.
#[derive(Clone, Copy, Debug, Default)]
pub struct StubAead;

impl StubAead {
    pub const fn forbidden_in_release() -> bool {
        !cfg!(debug_assertions)
    }

    fn keystream(key: &SecretBytes, nonce: &[u8], len: usize) -> Vec<u8> {
        let mut out = Vec::with_capacity(len + 32);
        let mut counter = 0u32;
        while out.len() < len {
            let mut hasher = Sha256::new();
            hasher.update(b"tf/keystore/stub-aead/v1\0");
            hasher.update(key.expose());
            hasher.update(nonce);
            hasher.update(counter.to_be_bytes());
            out.extend_from_slice(&hasher.finalize());
            counter += 1;
        }
        out.truncate(len);
        out
    }

    fn tag(key: &SecretBytes, aad: &[u8], ciphertext: &[u8]) -> [u8; 16] {
        let mut hasher = Sha256::new();
        hasher.update(b"tf/keystore/stub-tag/v1\0");
        hasher.update(key.expose());
        hasher.update((aad.len() as u64).to_be_bytes());
        hasher.update(aad);
        hasher.update((ciphertext.len() as u64).to_be_bytes());
        hasher.update(ciphertext);
        let digest = hasher.finalize();
        let mut tag = [0u8; 16];
        tag.copy_from_slice(&digest[..16]);
        tag
    }
}

impl Aead for StubAead {
    fn id(&self) -> u16 {
        0xeff1
    }

    fn name(&self) -> &'static str {
        "stub-aead (test only)"
    }

    fn seal(&self, key: &SecretBytes, key_id: u16, aad: &[u8], plain: &[u8]) -> Result<Vec<u8>> {
        debug_assert!(
            !Self::forbidden_in_release(),
            "StubAead must never be used from a release build"
        );
        debug_assert_eq!(key.len(), KEY_LEN, "StubAead needs a 32-byte key");
        if Self::forbidden_in_release() {
            return Err(Error::Unsupported(
                "StubAead is a test double and is disabled in release builds",
            ));
        }
        let nonce = random_nonce(AeadAlgorithm::XChaCha20Poly1305.nonce_len())?;
        let header = Header {
            algorithm: AeadAlgorithm::XChaCha20Poly1305,
            key_id,
            nonce: nonce.clone(),
        };
        let mut out = aead::encode_header(&header)?;
        let stream = Self::keystream(key, &nonce, plain.len());
        let mut ciphertext: Vec<u8> = plain
            .iter()
            .zip(stream.iter())
            .map(|(byte, key_byte)| byte ^ key_byte)
            .collect();
        let tag = Self::tag(key, aad, &ciphertext);
        out.append(&mut ciphertext);
        out.extend_from_slice(&tag);
        Ok(out)
    }

    fn open(&self, key: &SecretBytes, aad: &[u8], sealed: &[u8]) -> Result<SecretBytes> {
        debug_assert!(!Self::forbidden_in_release());
        if Self::forbidden_in_release() {
            return Err(Error::Unsupported(
                "StubAead is a test double and is disabled in release builds",
            ));
        }
        let (header, body_at) = aead::decode_header(sealed)?;
        let body = &sealed[body_at..];
        if body.len() < 16 {
            return Err(Error::Crypto("stub envelope is too short".to_string()));
        }
        let (ciphertext, tag) = body.split_at(body.len() - 16);
        let expected = Self::tag(key, aad, ciphertext);
        if !constant_time_eq(&expected, tag) {
            return Err(Error::Crypto(
                "stub envelope failed to open (wrong key or tampered)".to_string(),
            ));
        }
        let stream = Self::keystream(key, &header.nonce, ciphertext.len());
        let plain: Vec<u8> = ciphertext
            .iter()
            .zip(stream.iter())
            .map(|(byte, key_byte)| byte ^ key_byte)
            .collect();
        Ok(SecretBytes::new(plain))
    }
}

/// Cleartext metadata of one account's envelope. Contains no secret: the salt is
/// public by construction (Argon2id needs it), the wrapped DEK and the verifier
/// are ciphertext.
#[derive(Clone)]
pub struct Envelope {
    pub format_version: u32,
    pub account_id: AccountId,
    pub kdf_id: u16,
    pub aead_id: u16,
    pub cost: KdfCost,
    pub salt: [u8; SALT_LEN],
    pub key_id: u16,
    pub wrapped_dek: Vec<u8>,
    pub verifier: Vec<u8>,
    /// AEAD(recovery KEK, `… || b'r'`, DEK) when the account was created (or
    /// re-keyed) with a recovery code. `None` means "no recovery path", which is
    /// reported rather than silently accepted as a wrong code.
    pub wrapped_dek_recovery: Option<Vec<u8>>,
    pub created_at_ms: u64,
}

impl Envelope {
    /// `account_id || key_id` — the design's AAD, in that order and with no
    /// label, so an envelope moved to another account (or another key version)
    /// fails to open.
    pub fn aad(account_id: &AccountId, key_id: u16) -> Vec<u8> {
        let mut aad = Vec::with_capacity(AccountId::LEN + 2 + 1);
        aad.extend_from_slice(account_id.as_blob());
        aad.extend_from_slice(&key_id.to_be_bytes());
        aad
    }

    /// The AAD with the purpose byte appended (see the module docs).
    pub fn aad_for(account_id: &AccountId, key_id: u16, purpose: u8) -> Vec<u8> {
        let mut aad = Self::aad(account_id, key_id);
        aad.push(purpose);
        aad
    }

    pub fn to_json(&self) -> String {
        format!(
            concat!(
                "{{\"format_version\":{},\"account_id\":\"{}\",\"kdf\":{},\"aead\":{},",
                "\"m_cost_kib\":{},\"t_cost\":{},\"p_cost\":{},\"salt\":\"{}\",",
                "\"key_id\":{},\"wrapped_dek\":\"{}\",\"verifier\":\"{}\",",
                "\"wrapped_dek_recovery\":{},\"created_at_ms\":{}}}"
            ),
            self.format_version,
            self.account_id.to_hex(),
            self.kdf_id,
            self.aead_id,
            self.cost.m_cost_kib,
            self.cost.t_cost,
            self.cost.p_cost,
            crate::crypto::sig::to_hex(&self.salt),
            self.key_id,
            crate::crypto::sig::to_hex(&self.wrapped_dek),
            crate::crypto::sig::to_hex(&self.verifier),
            match &self.wrapped_dek_recovery {
                Some(wrapped) => format!("\"{}\"", crate::crypto::sig::to_hex(wrapped)),
                None => "null".to_string(),
            },
            self.created_at_ms
        )
    }

    /// Whether a recovery code can unlock this envelope.
    pub fn has_recovery(&self) -> bool {
        self.wrapped_dek_recovery.is_some()
    }

    /// Strict parser: every field is required, and a bad `salt`/`key_id` fails
    /// loudly instead of producing a weakened envelope.
    pub fn parse(text: &str) -> Result<Self> {
        use crate::api::protocol::{extract_number, extract_str};
        let format_version = extract_number(text, "format_version")
            .ok_or_else(|| Error::Invalid("envelope: no format_version".to_string()))?
            as u32;
        let account_hex = extract_str(text, "account_id")
            .ok_or_else(|| Error::Invalid("envelope: no account_id".to_string()))?;
        let account_id = AccountId::from_hex(&account_hex)?;
        let kdf_id = extract_number(text, "kdf")
            .ok_or_else(|| Error::Invalid("envelope: no kdf".to_string()))?
            as u16;
        let aead_id = extract_number(text, "aead")
            .ok_or_else(|| Error::Invalid("envelope: no aead".to_string()))?
            as u16;
        let cost = KdfCost {
            m_cost_kib: extract_number(text, "m_cost_kib")
                .ok_or_else(|| Error::Invalid("envelope: no m_cost_kib".to_string()))?
                as u32,
            t_cost: extract_number(text, "t_cost")
                .ok_or_else(|| Error::Invalid("envelope: no t_cost".to_string()))?
                as u32,
            p_cost: extract_number(text, "p_cost")
                .ok_or_else(|| Error::Invalid("envelope: no p_cost".to_string()))?
                as u32,
        };
        cost.validate()?;
        let salt_bytes = crate::crypto::sig::parse_hex(
            &extract_str(text, "salt")
                .ok_or_else(|| Error::Invalid("envelope: no salt".to_string()))?,
        )?;
        if salt_bytes.len() != SALT_LEN {
            return Err(Error::Invalid(format!(
                "envelope: salt must be {SALT_LEN} bytes, got {}",
                salt_bytes.len()
            )));
        }
        let mut salt = [0u8; SALT_LEN];
        salt.copy_from_slice(&salt_bytes);
        let key_id = extract_number(text, "key_id")
            .ok_or_else(|| Error::Invalid("envelope: no key_id".to_string()))?
            as u16;
        let wrapped_dek = crate::crypto::sig::parse_hex(
            &extract_str(text, "wrapped_dek")
                .ok_or_else(|| Error::Invalid("envelope: no wrapped_dek".to_string()))?,
        )?;
        let verifier = crate::crypto::sig::parse_hex(
            &extract_str(text, "verifier")
                .ok_or_else(|| Error::Invalid("envelope: no verifier".to_string()))?,
        )?;
        let created_at_ms = extract_number(text, "created_at_ms")
            .ok_or_else(|| Error::Invalid("envelope: no created_at_ms".to_string()))?;
        // Optional: an envelope written before recovery codes existed (or one
        // created without one) has no such field, and that is a real state, not an
        // error. `extract_str` returns `None` for `null` as well, which is what
        // `to_json` writes for `None`.
        let wrapped_dek_recovery = match extract_str(text, "wrapped_dek_recovery") {
            Some(hex) => Some(crate::crypto::sig::parse_hex(&hex)?),
            None => None,
        };
        Ok(Self {
            format_version,
            account_id,
            kdf_id,
            aead_id,
            cost,
            salt,
            key_id,
            wrapped_dek,
            verifier,
            wrapped_dek_recovery,
            created_at_ms,
        })
    }
}

impl core::fmt::Debug for Envelope {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        f.debug_struct("Envelope")
            .field("format_version", &self.format_version)
            .field("account_id", &self.account_id)
            .field("kdf_id", &self.kdf_id)
            .field("aead_id", &self.aead_id)
            .field("cost", &self.cost)
            .field("salt", &"<32 bytes>")
            .field("key_id", &self.key_id)
            .field(
                "wrapped_dek",
                &format_args!("<{} bytes>", self.wrapped_dek.len()),
            )
            .field("verifier", &format_args!("<{} bytes>", self.verifier.len()))
            .field(
                "wrapped_dek_recovery",
                &match &self.wrapped_dek_recovery {
                    Some(wrapped) => format!("<{} bytes>", wrapped.len()),
                    None => "<none>".to_string(),
                },
            )
            .finish()
    }
}

/// A DEK that has been unwrapped and will be wiped when dropped.
///
/// Not `Clone`: a second copy would escape the wipe. `Debug` prints nothing
/// derived from the key.
pub struct UnlockedSecret {
    account_id: AccountId,
    key_id: u16,
    dek: SecretBytes,
}

impl UnlockedSecret {
    pub fn account_id(&self) -> &AccountId {
        &self.account_id
    }

    pub fn key_id(&self) -> u16 {
        self.key_id
    }

    pub fn dek(&self) -> &SecretBytes {
        &self.dek
    }

    /// Labelled subkey, e.g. `kdf::LABEL_SYNC_DATA`. Derived on demand, never
    /// stored on disk.
    pub fn subkey(&self, label: &[u8]) -> Result<SecretBytes> {
        kdf::subkey(&self.dek, label)
    }

    /// Explicit lock: drop the key now instead of at end of scope.
    pub fn lock(self) {
        drop(self);
    }
}

impl core::fmt::Debug for UnlockedSecret {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        f.debug_struct("UnlockedSecret")
            .field("account_id", &self.account_id)
            .field("key_id", &self.key_id)
            .field("dek", &"<redacted>")
            .finish()
    }
}

/// Directory of per-account envelopes.
///
/// ```text
/// <dir>/accounts/<account_hex>.envelope.json   0600
/// ```
#[derive(Clone, Debug)]
pub struct SecretStore {
    dir: PathBuf,
}

impl SecretStore {
    pub fn new(dir: impl Into<PathBuf>) -> Self {
        Self { dir: dir.into() }
    }

    pub fn dir(&self) -> &Path {
        &self.dir
    }

    pub fn envelope_path(&self, account: &AccountId) -> PathBuf {
        self.dir
            .join("accounts")
            .join(format!("{}.envelope.json", account.to_hex()))
    }

    pub fn exists(&self, account: &AccountId) -> bool {
        self.envelope_path(account).exists()
    }

    /// Create an envelope with the default cost and XChaCha20-Poly1305.
    pub fn create(
        &self,
        account: &AccountId,
        passphrase: &SecretBytes,
    ) -> Result<(Envelope, UnlockedSecret)> {
        self.create_with(
            &Argon2Kdf,
            &PrimitiveAead::new(),
            account,
            passphrase,
            KdfCost::default(),
            crate::sync::clock::SystemClock.now_ms(),
        )
    }

    /// Create an envelope with injected primitives — the test/staging entry point.
    pub fn create_with(
        &self,
        kdf_impl: &dyn Kdf,
        aead_impl: &dyn Aead,
        account: &AccountId,
        passphrase: &SecretBytes,
        cost: KdfCost,
        now_ms: u64,
    ) -> Result<(Envelope, UnlockedSecret)> {
        let (envelope, unlocked, _) = self.create_inner(
            kdf_impl, aead_impl, account, passphrase, cost, now_ms, false,
        )?;
        Ok((envelope, unlocked))
    }

    /// Create an envelope **and** a recovery code that can unlock it without the
    /// passphrase.
    ///
    /// The code is returned exactly once and never written down: only the AEAD wrap
    /// of the DEK under a KEK derived from the code is stored, so the agent itself
    /// cannot recover it later. Losing both the passphrase and the code means losing
    /// the DEK, and that is deliberate — a third path would be a path an attacker can
    /// use as well.
    ///
    /// The recovery wrap always uses the real [`PrimitiveAead`] and real Argon2id,
    /// even when `kdf_impl`/`aead_impl` are stubs: there is no stub recovery path in
    /// production, so a stubbed recovery envelope would be a debug artifact that
    /// silently fails at the worst moment.
    pub fn create_with_recovery(
        &self,
        kdf_impl: &dyn Kdf,
        aead_impl: &dyn Aead,
        account: &AccountId,
        passphrase: &SecretBytes,
        cost: KdfCost,
        now_ms: u64,
    ) -> Result<(Envelope, UnlockedSecret, SecretBytes)> {
        let (envelope, unlocked, code) =
            self.create_inner(kdf_impl, aead_impl, account, passphrase, cost, now_ms, true)?;
        let code = code.ok_or_else(|| {
            Error::Crypto("recovery was requested but no code was generated".to_string())
        })?;
        Ok((envelope, unlocked, code))
    }

    #[allow(clippy::too_many_arguments)]
    fn create_inner(
        &self,
        kdf_impl: &dyn Kdf,
        aead_impl: &dyn Aead,
        account: &AccountId,
        passphrase: &SecretBytes,
        cost: KdfCost,
        now_ms: u64,
        with_recovery: bool,
    ) -> Result<(Envelope, UnlockedSecret, Option<SecretBytes>)> {
        kdf::validate_passphrase(passphrase)?;
        cost.validate()?;
        if self.exists(account) {
            return Err(Error::Invalid(format!(
                "an envelope for account {} already exists",
                account.to_hex()
            )));
        }

        let mut salt = [0u8; SALT_LEN];
        fill_random(&mut salt)?;
        let kek = kdf_impl.derive(passphrase, &salt, cost)?;

        // The DEK must be random, never derived from the passphrase: that is what
        // makes `change_passphrase` a 32-byte re-wrap instead of a re-encryption.
        let dek = SecretBytes::generate(KEY_LEN)?;
        let key_id = 1u16;
        let dek_aad = Envelope::aad_for(account, key_id, PURPOSE_DEK);
        let verifier_aad = Envelope::aad_for(account, key_id, PURPOSE_VERIFIER);
        let wrapped_dek = aead_impl.seal(&kek, key_id, &dek_aad, dek.expose())?;
        let verifier = aead_impl.seal(&kek, key_id, &verifier_aad, VERIFIER_PLAINTEXT)?;

        let (wrapped_dek_recovery, recovery_code) = if with_recovery {
            let code = SecretBytes::generate(RECOVERY_CODE_LEN)?;
            let params = kdf::KdfParams {
                algorithm: kdf::KdfAlgorithm::Argon2idV13,
                cost,
                salt,
            };
            let recovery_kek = kdf::derive_recovery_kek(&code, &params)?;
            let recovery_aad = Envelope::aad_for(account, key_id, PURPOSE_RECOVERY);
            let wrapped =
                PrimitiveAead::new().seal(&recovery_kek, key_id, &recovery_aad, dek.expose())?;
            (Some(wrapped), Some(code))
        } else {
            (None, None)
        };

        let envelope = Envelope {
            format_version: FORMAT_VERSION,
            account_id: *account,
            kdf_id: kdf_impl.id(),
            aead_id: aead_impl.id(),
            cost,
            salt,
            key_id,
            wrapped_dek,
            verifier,
            wrapped_dek_recovery,
            created_at_ms: now_ms,
        };
        self.write(&envelope)?;

        Ok((
            envelope,
            UnlockedSecret {
                account_id: *account,
                key_id,
                dek,
            },
            recovery_code,
        ))
    }

    /// Unlock with the recovery code instead of the passphrase.
    ///
    /// Wrong code and missing recovery envelope are both [`Error::Auth`], but with
    /// different messages: the first is "you typed it wrong", the second is "this
    /// account never had one" — the client can act differently on those, and neither
    /// reveals anything about the DEK.
    pub fn unlock_with_recovery(
        &self,
        account: &AccountId,
        code: &SecretBytes,
        envelope: &Envelope,
    ) -> Result<UnlockedSecret> {
        if envelope.format_version != FORMAT_VERSION {
            return Err(Error::Invalid(format!(
                "envelope format {} (this build speaks {FORMAT_VERSION})",
                envelope.format_version
            )));
        }
        if envelope.account_id != *account {
            return Err(Error::Auth(
                "this envelope belongs to another account".to_string(),
            ));
        }
        let wrapped = envelope
            .wrapped_dek_recovery
            .as_ref()
            .ok_or_else(|| Error::Auth("this account has no recovery envelope".to_string()))?;
        if code.len() < RECOVERY_CODE_LEN {
            return Err(Error::Auth("wrong recovery code".to_string()));
        }
        let params = kdf::KdfParams {
            algorithm: kdf::KdfAlgorithm::Argon2idV13,
            cost: envelope.cost,
            salt: envelope.salt,
        };
        let recovery_kek = kdf::derive_recovery_kek(code, &params)?;
        let aad = Envelope::aad_for(&envelope.account_id, envelope.key_id, PURPOSE_RECOVERY);
        let dek = PrimitiveAead::new()
            .open(&recovery_kek, &aad, wrapped)
            .map_err(|_| Error::Auth("wrong recovery code".to_string()))?;
        if dek.len() != KEY_LEN {
            return Err(Error::Crypto(format!(
                "unwrapped DEK has {} bytes, expected {KEY_LEN}",
                dek.len()
            )));
        }
        Ok(UnlockedSecret {
            account_id: envelope.account_id,
            key_id: envelope.key_id,
            dek,
        })
    }
    /// Unlock with the default primitives.
    pub fn unlock(&self, account: &AccountId, passphrase: &SecretBytes) -> Result<UnlockedSecret> {
        let envelope = self.read(account)?;
        if envelope.kdf_id != Argon2Kdf.id() {
            return Err(Error::Unsupported(
                "the envelope names a kdf this build does not implement",
            ));
        }
        let aead_impl = PrimitiveAead::new();
        self.unlock_with(&Argon2Kdf, &aead_impl, account, passphrase, &envelope)
    }

    /// Unlock with injected primitives.
    pub fn unlock_with(
        &self,
        kdf_impl: &dyn Kdf,
        aead_impl: &dyn Aead,
        account: &AccountId,
        passphrase: &SecretBytes,
        envelope: &Envelope,
    ) -> Result<UnlockedSecret> {
        if envelope.format_version != FORMAT_VERSION {
            return Err(Error::Invalid(format!(
                "envelope format {} (this build speaks {FORMAT_VERSION})",
                envelope.format_version
            )));
        }
        if envelope.account_id != *account {
            return Err(Error::Auth(
                "this envelope belongs to another account".to_string(),
            ));
        }
        let kek = kdf_impl.derive(passphrase, &envelope.salt, envelope.cost)?;

        // The verifier is opened first: a wrong passphrase fails here, and the
        // error says nothing about whether the passphrase or the file was wrong.
        let verifier_aad =
            Envelope::aad_for(&envelope.account_id, envelope.key_id, PURPOSE_VERIFIER);
        let opened = aead_impl
            .open(&kek, &verifier_aad, &envelope.verifier)
            .map_err(|_| Error::Auth("wrong passphrase".to_string()))?;
        if !constant_time_eq(opened.expose(), VERIFIER_PLAINTEXT) {
            return Err(Error::Auth("wrong passphrase".to_string()));
        }

        let dek_aad = Envelope::aad_for(&envelope.account_id, envelope.key_id, PURPOSE_DEK);
        let dek = aead_impl.open(&kek, &dek_aad, &envelope.wrapped_dek)?;
        if dek.len() != KEY_LEN {
            return Err(Error::Crypto(format!(
                "unwrapped DEK has {} bytes, expected {KEY_LEN}",
                dek.len()
            )));
        }
        Ok(UnlockedSecret {
            account_id: envelope.account_id,
            key_id: envelope.key_id,
            dek,
        })
    }

    pub fn read(&self, account: &AccountId) -> Result<Envelope> {
        let path = self.envelope_path(account);
        let text = std::fs::read_to_string(&path).map_err(|error| {
            Error::Io(std::io::Error::new(
                error.kind(),
                format!("cannot read {}: {error}", path.display()),
            ))
        })?;
        Envelope::parse(&text)
    }

    /// Write the envelope with `0600`, atomically (temp file + `rename`).
    pub fn write(&self, envelope: &Envelope) -> Result<()> {
        let directory = self.dir.join("accounts");
        fsperm::create_private_dir(&directory.to_string_lossy())?;
        let path = self.envelope_path(&envelope.account_id);
        fsperm::write_private_file_replace(&path.to_string_lossy(), envelope.to_json().as_bytes())
    }

    /// Bump `key_id` and re-wrap the same DEK under a fresh salt.
    ///
    /// The DEK does not change, so no stored value has to be re-encrypted. The
    /// old envelope file is atomically replaced; the previous `key_id` is gone, so
    /// a blob sealed under it is distinguishable by its `key_id` in the header.
    pub fn rotate(
        &self,
        account: &AccountId,
        old_passphrase: &SecretBytes,
        new_passphrase: &SecretBytes,
    ) -> Result<Envelope> {
        self.rotate_with(
            &Argon2Kdf,
            &PrimitiveAead::new(),
            account,
            old_passphrase,
            new_passphrase,
        )
    }

    /// [`Self::rotate`] with injected primitives.
    pub fn rotate_with(
        &self,
        kdf_impl: &dyn Kdf,
        aead_impl: &dyn Aead,
        account: &AccountId,
        old_passphrase: &SecretBytes,
        new_passphrase: &SecretBytes,
    ) -> Result<Envelope> {
        let mut envelope = self.read(account)?;
        if envelope.has_recovery() {
            // Re-wrapping the recovery envelope needs the code itself, which the
            // store does not have. Refusing is the documented behaviour: silently
            // dropping the recovery wrap would turn "changed my password" into
            // "lost my only way back in" without telling anyone.
            return Err(Error::Unsupported(
                "re-wrapping the recovery envelope needs the recovery code; rotate the recovery code first",
            ));
        }
        if envelope.kdf_id != kdf_impl.id() {
            return Err(Error::Unsupported(
                "the envelope names a kdf that does not match the injected one",
            ));
        }
        let unlocked = self.unlock_with(kdf_impl, aead_impl, account, old_passphrase, &envelope)?;
        kdf::validate_passphrase(new_passphrase)?;

        let mut salt = [0u8; SALT_LEN];
        fill_random(&mut salt)?;
        let kek = kdf_impl.derive(new_passphrase, &salt, envelope.cost)?;
        let key_id = envelope
            .key_id
            .checked_add(1)
            .ok_or_else(|| Error::Crypto("key_id overflow".to_string()))?;
        let dek_aad = Envelope::aad_for(account, key_id, PURPOSE_DEK);
        let verifier_aad = Envelope::aad_for(account, key_id, PURPOSE_VERIFIER);

        envelope.salt = salt;
        envelope.key_id = key_id;
        envelope.wrapped_dek = aead_impl.seal(&kek, key_id, &dek_aad, unlocked.dek().expose())?;
        envelope.verifier = aead_impl.seal(&kek, key_id, &verifier_aad, VERIFIER_PLAINTEXT)?;
        self.write(&envelope)?;
        Ok(envelope)
    }

    pub fn remove(&self, account: &AccountId) -> Result<()> {
        let path = self.envelope_path(account);
        match std::fs::remove_file(&path) {
            Ok(()) => Ok(()),
            Err(error) if error.kind() == std::io::ErrorKind::NotFound => Ok(()),
            Err(error) => Err(Error::Io(error)),
        }
    }
}

fn fill_random(buf: &mut [u8]) -> Result<()> {
    use rand::RngCore;
    rand::rngs::OsRng.fill_bytes(buf);
    Ok(())
}

fn random_nonce(len: usize) -> Result<Vec<u8>> {
    let mut nonce = vec![0u8; len];
    fill_random(&mut nonce)?;
    Ok(nonce)
}

/// `self-test` probe: prove that the *real* KDF and AEAD actually work in this
/// build, without needing a passphrase or an existing tenant.
///
/// The KDF part runs at [`KdfCost::floor`] — about 40 ms and 32 MiB — because the
/// point is to catch "the primitive is a stub / the CPU feature is missing", not to
/// benchmark.
pub fn probe() -> Result<()> {
    let key = SecretBytes::generate(KEY_LEN)?;
    let aead_impl = PrimitiveAead::new();
    let aad = b"tf/self-test/aead";
    let sealed = aead_impl.seal(&key, 1, aad, b"probe")?;
    let opened = aead_impl.open(&key, aad, &sealed)?;
    if opened.expose() != b"probe" {
        return Err(Error::Crypto(
            "AEAD roundtrip returned different plaintext".to_string(),
        ));
    }
    if aead_impl
        .open(&SecretBytes::generate(KEY_LEN)?, aad, &sealed)
        .is_ok()
    {
        return Err(Error::Crypto(
            "AEAD accepted the wrong key; the primitive is not authenticating".to_string(),
        ));
    }

    let passphrase = SecretBytes::new(b"self-test passphrase".to_vec());
    let salt = [0x5au8; SALT_LEN];
    let kek = Argon2Kdf.derive(&passphrase, &salt, KdfCost::floor())?;
    if kek.len() != KEY_LEN {
        return Err(Error::Crypto(format!(
            "Argon2id produced {} bytes, expected {KEY_LEN}",
            kek.len()
        )));
    }
    let again = Argon2Kdf.derive(&passphrase, &salt, KdfCost::floor())?;
    if kek.expose() != again.expose() {
        return Err(Error::Crypto(
            "Argon2id is not deterministic for the same salt".to_string(),
        ));
    }
    let other = Argon2Kdf.derive(
        &SecretBytes::new(b"different passphrase".to_vec()),
        &salt,
        KdfCost::floor(),
    )?;
    if kek.expose() == other.expose() {
        return Err(Error::Crypto("Argon2id ignored the passphrase".to_string()));
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn account(byte: u8) -> AccountId {
        AccountId::new([byte; 16]).unwrap()
    }

    fn passphrase(text: &str) -> SecretBytes {
        SecretBytes::new(text.as_bytes().to_vec())
    }

    fn store() -> (crate::test_support::TestDir, SecretStore) {
        let directory = crate::test_support::TestDir::new("keystore");
        let store = SecretStore::new(directory.join("keystore"));
        (directory, store)
    }

    fn stub_store() -> (crate::test_support::TestDir, SecretStore) {
        store()
    }

    fn create_stub(
        store: &SecretStore,
        account: &AccountId,
        passphrase: &SecretBytes,
    ) -> Result<(Envelope, UnlockedSecret)> {
        store.create_with(
            &StubKdf,
            &StubAead,
            account,
            passphrase,
            KdfCost::floor(),
            1_700_000_000_000,
        )
    }

    #[test]
    fn create_then_unlock_returns_the_same_dek() {
        let (_guard, store) = stub_store();
        let account = account(1);
        let (envelope, unlocked) =
            create_stub(&store, &account, &passphrase("correct horse battery")).unwrap();
        assert_eq!(envelope.key_id, 1);
        assert_eq!(envelope.format_version, FORMAT_VERSION);
        assert_eq!(unlocked.dek().len(), KEY_LEN);

        let reopened = store
            .unlock_with(
                &StubKdf,
                &StubAead,
                &account,
                &passphrase("correct horse battery"),
                &envelope,
            )
            .unwrap();
        assert_eq!(reopened.dek().expose(), unlocked.dek().expose());
        assert_eq!(reopened.account_id(), &account);
    }

    #[test]
    fn a_wrong_passphrase_fails_and_says_nothing_useful() {
        let (_guard, store) = stub_store();
        let account = account(2);
        let (envelope, _) =
            create_stub(&store, &account, &passphrase("correct horse battery")).unwrap();
        let error = store
            .unlock_with(
                &StubKdf,
                &StubAead,
                &account,
                &passphrase("wrong horse battery"),
                &envelope,
            )
            .unwrap_err();
        assert_eq!(error.exit_code(), crate::error::exit::AUTH);
        assert!(error.to_string().contains("wrong passphrase"));
        assert!(!error.to_string().contains("correct horse"));
    }

    #[test]
    fn the_same_passphrase_produces_independent_keys_per_account() {
        let (_guard, store) = stub_store();
        let shared = passphrase("correct horse battery");
        let (first, first_unlocked) = create_stub(&store, &account(3), &shared).unwrap();
        let (second, second_unlocked) = create_stub(&store, &account(4), &shared).unwrap();
        assert_ne!(
            first.salt, second.salt,
            "the salt must be random per account"
        );
        assert_ne!(
            first_unlocked.dek().expose(),
            second_unlocked.dek().expose(),
            "the DEK must be random, not derived from the passphrase"
        );
        assert_ne!(first.wrapped_dek, second.wrapped_dek);
    }

    #[test]
    fn an_envelope_cannot_be_moved_to_another_account() {
        // The AAD is `account_id || key_id`, so both a copied file and an edited
        // `account_id` field must fail: the first is caught by the explicit
        // ownership check, the second by the AEAD tag.
        let (_guard, store) = stub_store();
        let owner = account(5);
        let thief = account(6);
        let secret = passphrase("correct horse battery");
        let (owner_envelope, _) = create_stub(&store, &owner, &secret).unwrap();
        create_stub(&store, &thief, &secret).unwrap(); // so the thief has a slot

        // 1. Copy the owner's envelope into the thief's slot.
        std::fs::copy(store.envelope_path(&owner), store.envelope_path(&thief)).unwrap();
        let stolen = store.read(&thief).unwrap();
        let error = store
            .unlock_with(&StubKdf, &StubAead, &thief, &secret, &stolen)
            .unwrap_err();
        assert!(
            error.to_string().contains("another account"),
            "unexpected error: {error}"
        );

        // 2. Forge the account_id field and retry: now the AAD no longer matches
        //    the ciphertext, so the verifier fails to open.
        let mut forged = owner_envelope;
        forged.account_id = thief;
        let error = store
            .unlock_with(&StubKdf, &StubAead, &thief, &secret, &forged)
            .unwrap_err();
        assert_eq!(error.exit_code(), crate::error::exit::AUTH);
    }

    #[test]
    fn a_key_id_bump_invalidates_the_old_wrap() {
        let (_guard, store) = stub_store();
        let account = account(7);
        let (mut envelope, _) =
            create_stub(&store, &account, &passphrase("correct horse battery")).unwrap();
        envelope.key_id = 2;
        assert!(store
            .unlock_with(
                &StubKdf,
                &StubAead,
                &account,
                &passphrase("correct horse battery"),
                &envelope
            )
            .is_err());
    }

    #[test]
    fn envelope_json_roundtrips_and_contains_no_secret() {
        let (_guard, store) = stub_store();
        let account = account(8);
        let (envelope, unlocked) =
            create_stub(&store, &account, &passphrase("correct horse battery")).unwrap();
        let json = envelope.to_json();
        assert!(!json.contains("correct horse"));
        assert!(!json.contains(&crate::crypto::sig::to_hex(unlocked.dek().expose())));

        let parsed = Envelope::parse(&json).unwrap();
        assert_eq!(parsed.account_id, envelope.account_id);
        assert_eq!(parsed.salt, envelope.salt);
        assert_eq!(parsed.key_id, envelope.key_id);
        assert_eq!(parsed.wrapped_dek, envelope.wrapped_dek);
        assert_eq!(parsed.cost, envelope.cost);
        assert_eq!(parsed.to_json(), json);
    }

    #[test]
    fn envelope_parse_rejects_incomplete_or_downgraded_metadata() {
        let (_guard, store) = stub_store();
        let account = account(9);
        let (envelope, _) =
            create_stub(&store, &account, &passphrase("correct horse battery")).unwrap();
        let json = envelope.to_json();

        let no_salt = json.replace(&crate::crypto::sig::to_hex(&envelope.salt), "");
        assert!(Envelope::parse(&no_salt).is_err());

        let downgraded = json.replace("\"m_cost_kib\":32768", "\"m_cost_kib\":8");
        assert!(
            Envelope::parse(&downgraded).is_err(),
            "a downgraded cost must not parse"
        );

        let short_salt = json.replace(&crate::crypto::sig::to_hex(&envelope.salt), "0011");
        assert!(Envelope::parse(&short_salt).is_err());
        assert!(Envelope::parse("{}").is_err());
    }

    #[test]
    fn debug_output_never_contains_the_passphrase_or_the_dek() {
        let (_guard, store) = stub_store();
        let account = account(10);
        let secret_text = "correct horse battery staple";
        let (envelope, unlocked) = create_stub(&store, &account, &passphrase(secret_text)).unwrap();
        let dek_hex = crate::crypto::sig::to_hex(unlocked.dek().expose());

        for rendered in [
            format!("{envelope:?}"),
            format!("{unlocked:?}"),
            format!("{:?}", passphrase(secret_text)),
            format!("{store:?}"),
        ] {
            assert!(
                !rendered.contains(secret_text),
                "debug output leaked the passphrase: {rendered}"
            );
            assert!(
                !rendered.contains(&dek_hex),
                "debug output leaked the DEK: {rendered}"
            );
        }
        assert!(format!("{unlocked:?}").contains("<redacted>"));
        assert!(format!("{envelope:?}").contains("<32 bytes>"));
    }

    #[test]
    fn the_file_is_created_with_private_permissions() {
        let (_guard, store) = stub_store();
        let account = account(11);
        let (envelope, _) =
            create_stub(&store, &account, &passphrase("correct horse battery")).unwrap();
        let path = store.envelope_path(&account);
        assert!(path.exists());
        let on_disk = std::fs::read_to_string(&path).unwrap();
        assert_eq!(on_disk, envelope.to_json());

        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            let mode = std::fs::metadata(&path).unwrap().permissions().mode() & 0o777;
            assert_eq!(mode, fsperm::FILE_PRIVATE, "envelope must be 0600");
        }
        #[cfg(not(unix))]
        {
            // There are no POSIX mode bits on Windows; `fsperm` enforces the
            // equivalent DACL and reports whether it can *verify* it.
            assert!(!fsperm::posix_modes_enforced());
        }
    }

    #[test]
    fn create_refuses_to_overwrite_an_existing_envelope() {
        let (_guard, store) = stub_store();
        let account = account(12);
        create_stub(&store, &account, &passphrase("correct horse battery")).unwrap();
        let error = create_stub(&store, &account, &passphrase("another passphrase")).unwrap_err();
        assert!(error.to_string().contains("already exists"));
    }

    #[test]
    fn short_passphrases_and_downgraded_costs_are_refused() {
        let (_guard, store) = stub_store();
        let account = account(13);
        assert!(store
            .create_with(
                &StubKdf,
                &StubAead,
                &account,
                &passphrase("short"),
                KdfCost::floor(),
                0
            )
            .is_err());
        assert!(store
            .create_with(
                &StubKdf,
                &StubAead,
                &account,
                &passphrase("correct horse battery"),
                KdfCost {
                    m_cost_kib: 8,
                    t_cost: 1,
                    p_cost: 1
                },
                0
            )
            .is_err());
        assert!(!store.exists(&account));
    }

    #[test]
    fn rotate_bumps_the_key_id_and_keeps_the_dek() {
        let (_guard, store) = stub_store();
        let account = account(14);
        let (envelope, unlocked) =
            create_stub(&store, &account, &passphrase("correct horse battery")).unwrap();

        let rotated = store
            .rotate_with(
                &StubKdf,
                &StubAead,
                &account,
                &passphrase("correct horse battery"),
                &passphrase("a brand new passphrase"),
            )
            .unwrap();
        assert_eq!(rotated.key_id, envelope.key_id + 1);
        assert_ne!(rotated.salt, envelope.salt);
        assert_eq!(rotated.cost, envelope.cost);

        // The old passphrase no longer opens it ...
        assert!(store
            .unlock_with(
                &StubKdf,
                &StubAead,
                &account,
                &passphrase("correct horse battery"),
                &rotated
            )
            .is_err());
        // ... the new one does, and the DEK is unchanged: a passphrase change is a
        // 32-byte re-wrap, not a re-encryption of the data.
        let reopened = store
            .unlock_with(
                &StubKdf,
                &StubAead,
                &account,
                &passphrase("a brand new passphrase"),
                &rotated,
            )
            .unwrap();
        assert_eq!(reopened.dek().expose(), unlocked.dek().expose());
        assert_eq!(reopened.key_id(), rotated.key_id);
    }

    #[test]
    fn remove_is_idempotent() {
        let (_guard, store) = stub_store();
        let account = account(15);
        create_stub(&store, &account, &passphrase("correct horse battery")).unwrap();
        assert!(store.exists(&account));
        store.remove(&account).unwrap();
        assert!(!store.exists(&account));
        store.remove(&account).unwrap();
    }

    #[test]
    fn stub_primitives_are_deterministic_but_still_authenticate() {
        let key = SecretBytes::new(vec![7u8; KEY_LEN]);
        let salt = [3u8; SALT_LEN];
        let cost = KdfCost::floor();
        let a = StubKdf
            .derive(&passphrase("correct horse battery"), &salt, cost)
            .unwrap();
        let b = StubKdf
            .derive(&passphrase("correct horse battery"), &salt, cost)
            .unwrap();
        assert_eq!(a.expose(), b.expose());
        let c = StubKdf
            .derive(&passphrase("correct horse battery"), &[4u8; SALT_LEN], cost)
            .unwrap();
        assert_ne!(a.expose(), c.expose());

        let sealed = StubAead.seal(&key, 1, b"aad", b"plain").unwrap();
        assert_eq!(
            StubAead.open(&key, b"aad", &sealed).unwrap().expose(),
            b"plain"
        );
        assert!(StubAead.open(&key, b"other", &sealed).is_err());
        let mut tampered = sealed.clone();
        let last = tampered.len() - 1;
        tampered[last] ^= 0xff;
        assert!(StubAead.open(&key, b"aad", &tampered).is_err());
    }

    #[test]
    fn stub_aead_output_is_not_readable_by_the_real_aead() {
        let key = SecretBytes::new(vec![7u8; KEY_LEN]);
        let sealed = StubAead.seal(&key, 1, b"aad", b"plain").unwrap();
        assert!(
            PrimitiveAead::new().open(&key, b"aad", &sealed).is_err(),
            "a test-double envelope must fail closed against the real primitive"
        );
    }

    #[test]
    fn real_primitives_roundtrip_through_the_envelope() {
        let (_guard, store) = stub_store();
        let account = account(16);
        let (envelope, unlocked) = store
            .create_with(
                &Argon2Kdf,
                &PrimitiveAead::new(),
                &account,
                &passphrase("correct horse battery"),
                KdfCost::floor(),
                7,
            )
            .unwrap();
        assert_eq!(envelope.kdf_id, Argon2Kdf.id());
        assert_eq!(envelope.aead_id, 1);
        // The production path must be able to open what the production path wrote.
        let reopened = store
            .unlock_with(
                &Argon2Kdf,
                &PrimitiveAead::new(),
                &account,
                &passphrase("correct horse battery"),
                &envelope,
            )
            .unwrap();
        assert_eq!(reopened.dek().expose(), unlocked.dek().expose());
    }

    #[test]
    fn subkeys_are_labelled_and_differ() {
        let (_guard, store) = stub_store();
        let account = account(17);
        let (_envelope, unlocked) =
            create_stub(&store, &account, &passphrase("correct horse battery")).unwrap();
        let sync = unlocked
            .subkey(crate::crypto::kdf::LABEL_SYNC_DATA)
            .unwrap();
        let blind = unlocked.subkey(crate::crypto::kdf::LABEL_BLIND).unwrap();
        assert_eq!(sync.len(), KEY_LEN);
        assert_ne!(sync.expose(), blind.expose());
    }

    #[test]
    fn aad_is_account_then_key_id_then_purpose() {
        let account = account(18);
        let base = Envelope::aad(&account, 0x0102);
        assert_eq!(base.len(), AccountId::LEN + 2);
        assert_eq!(&base[..16], account.as_blob());
        assert_eq!(&base[16..], &[0x01, 0x02]);
        let with_purpose = Envelope::aad_for(&account, 0x0102, PURPOSE_DEK);
        assert_eq!(with_purpose.len(), base.len() + 1);
        assert_eq!(*with_purpose.last().unwrap(), PURPOSE_DEK);
        assert_ne!(
            Envelope::aad_for(&account, 0x0102, PURPOSE_DEK),
            Envelope::aad_for(&account, 0x0102, PURPOSE_VERIFIER)
        );
    }

    #[test]
    fn probe_passes_with_the_real_primitives() {
        probe().unwrap();
    }

    #[test]
    fn a_verifier_moved_into_the_dek_slot_does_not_unwrap_a_key() {
        // Same KEK, same account, same key_id: only the purpose byte separates
        // the two blobs.
        let (_guard, store) = stub_store();
        let account = account(19);
        let (mut envelope, _) =
            create_stub(&store, &account, &passphrase("correct horse battery")).unwrap();
        let verifier = envelope.verifier.clone();
        envelope.wrapped_dek = verifier;
        assert!(store
            .unlock_with(
                &StubKdf,
                &StubAead,
                &account,
                &passphrase("correct horse battery"),
                &envelope
            )
            .is_err());
    }

    #[test]
    fn missing_envelope_is_an_io_error_with_the_path() {
        let (_guard, store) = stub_store();
        let error = store.read(&account(20)).unwrap_err();
        assert!(error.to_string().contains("envelope.json"));
    }

    /// Real primitives at the cost floor: recovery is a production path with no stub
    /// variant, so its tests must not use one.
    fn create_real(
        store: &SecretStore,
        account: &AccountId,
        passphrase: &SecretBytes,
    ) -> (Envelope, UnlockedSecret, SecretBytes) {
        store
            .create_with_recovery(
                &Argon2Kdf,
                &PrimitiveAead::new(),
                account,
                passphrase,
                KdfCost::floor(),
                1_700_000_000_000,
            )
            .expect("an envelope with a recovery code")
    }

    #[test]
    fn a_recovery_code_unlocks_the_dek_without_the_passphrase() {
        let (_guard, store) = store();
        let account = account(21);
        let (envelope, unlocked, code) =
            create_real(&store, &account, &passphrase("correct horse battery"));
        assert!(envelope.has_recovery());
        assert_eq!(code.len(), RECOVERY_CODE_LEN);
        assert_ne!(code.expose(), b"correct horse battery");

        let recovered = store
            .unlock_with_recovery(&account, &code, &envelope)
            .expect("the printed code unlocks the account");
        assert_eq!(recovered.dek().expose(), unlocked.dek().expose());
        assert_eq!(recovered.account_id(), &account);
        assert_eq!(recovered.key_id(), unlocked.key_id());

        // ... and a subkey is the same, so every stored value still opens.
        let label = kdf::LABEL_SYNC_DATA;
        assert_eq!(
            recovered.subkey(label).unwrap().expose(),
            unlocked.subkey(label).unwrap().expose()
        );
    }

    #[test]
    fn a_wrong_recovery_code_is_refused() {
        let (_guard, store) = store();
        let account = account(22);
        let (envelope, _, _) = create_real(&store, &account, &passphrase("correct horse battery"));
        let error = store
            .unlock_with_recovery(
                &account,
                &SecretBytes::new(vec![0x00; RECOVERY_CODE_LEN]),
                &envelope,
            )
            .unwrap_err();
        assert_eq!(error.exit_code(), crate::error::exit::AUTH);
        assert!(error.to_string().contains("wrong recovery code"));

        // A short code cannot even reach the AEAD; it is refused the same way so the
        // error does not distinguish "short" from "wrong".
        let short = store
            .unlock_with_recovery(&account, &SecretBytes::new(vec![1, 2, 3]), &envelope)
            .unwrap_err();
        assert!(short.to_string().contains("wrong recovery code"));
    }

    #[test]
    fn an_account_without_a_recovery_envelope_says_so() {
        let (_guard, store) = stub_store();
        let account = account(23);
        let (envelope, _) =
            create_stub(&store, &account, &passphrase("correct horse battery")).unwrap();
        assert!(!envelope.has_recovery());
        let error = store
            .unlock_with_recovery(
                &account,
                &SecretBytes::new(vec![7; RECOVERY_CODE_LEN]),
                &envelope,
            )
            .unwrap_err();
        assert!(
            error.to_string().contains("no recovery envelope"),
            "{error}"
        );
    }

    #[test]
    fn a_recovery_wrap_cannot_be_moved_into_the_passphrase_slot() {
        // The purpose byte in the AAD is what stops the substitution: without it the
        // same KEK would open a blob placed in the other slot.
        let (_guard, store) = store();
        let account = account(24);
        let (mut envelope, _, code) =
            create_real(&store, &account, &passphrase("correct horse battery"));
        let recovery = envelope
            .wrapped_dek_recovery
            .clone()
            .expect("recovery wrap");
        envelope.wrapped_dek = recovery;
        // Even with the correct passphrase, the passphrase slot now holds a blob
        // sealed under the recovery KEK, so the verifier still passes but the DEK
        // unwrap must fail.
        let error = store
            .unlock_with(
                &Argon2Kdf,
                &PrimitiveAead::new(),
                &account,
                &passphrase("correct horse battery"),
                &envelope,
            )
            .unwrap_err();
        assert!(
            matches!(error, Error::Crypto(_) | Error::Auth(_)),
            "{error}"
        );
        // And the code still works on the untouched recovery slot.
        assert!(store
            .unlock_with_recovery(&account, &code, &envelope)
            .is_ok());
    }

    #[test]
    fn a_recovery_envelope_survives_a_json_roundtrip() {
        let (_guard, store) = store();
        let owner = account(25);
        let (envelope, _, code) = create_real(&store, &owner, &passphrase("correct horse battery"));
        let reparsed = Envelope::parse(&envelope.to_json()).unwrap();
        assert_eq!(reparsed.wrapped_dek_recovery, envelope.wrapped_dek_recovery);
        assert!(reparsed.has_recovery());
        assert_eq!(
            store
                .unlock_with_recovery(&owner, &code, &reparsed)
                .unwrap()
                .dek()
                .expose(),
            store
                .unlock_with_recovery(&owner, &code, &envelope)
                .unwrap()
                .dek()
                .expose()
        );

        // An envelope written without one still parses (no field / null).
        let (plain, _) =
            create_stub(&store, &account(26), &passphrase("correct horse battery")).unwrap();
        let reparsed = Envelope::parse(&plain.to_json()).unwrap();
        assert!(!reparsed.has_recovery());
        assert!(Envelope::parse(
            &plain
                .to_json()
                .replace("\"wrapped_dek_recovery\":null,", "")
        )
        .is_ok());
    }

    #[test]
    fn rotating_refuses_to_silently_drop_the_recovery_wrap() {
        let (_guard, store) = store();
        let account = account(27);
        let (_, _, _) = create_real(&store, &account, &passphrase("correct horse battery"));
        let error = store
            .rotate_with(
                &Argon2Kdf,
                &PrimitiveAead::new(),
                &account,
                &passphrase("correct horse battery"),
                &passphrase("new correct horse battery"),
            )
            .unwrap_err();
        assert!(error.to_string().contains("recovery"), "{error}");
        // The envelope is untouched: the old passphrase still opens it.
        assert!(store
            .unlock_with(
                &Argon2Kdf,
                &PrimitiveAead::new(),
                &account,
                &passphrase("correct horse battery"),
                &store.read(&account).unwrap(),
            )
            .is_ok());
    }

    #[test]
    fn two_recovery_codes_are_independent_and_debug_hides_them() {
        let (_guard, store) = store();
        let (first, _, first_code) =
            create_real(&store, &account(28), &passphrase("correct horse battery"));
        let (second, _, second_code) =
            create_real(&store, &account(29), &passphrase("correct horse battery"));
        assert_ne!(first_code.expose(), second_code.expose());
        assert_ne!(first.wrapped_dek_recovery, second.wrapped_dek_recovery);
        let rendered = format!("{first:?}");
        assert!(!rendered.contains(&crate::crypto::sig::to_hex(
            &first.wrapped_dek_recovery.clone().unwrap()
        )));
        assert!(rendered.contains("<"));
    }
}
