//! Password-based key derivation and subkey separation.
//!
//! # Why the current C++ scheme is not encryption
//!
//! `teleforge_sync.cpp` builds the cloud key like this:
//!
//! ```text
//! MasterMaterial() = QString::number(session->user()->id.value) + session->user()->phone()
//! SyncKey()        = DeriveSyncKey(MasterMaterial(), g_exportKey())
//! ```
//!
//! and `teleforge_sync_crypto.cpp` resolves `DeriveSyncKey` with an empty
//! `g_exportKey` (it is never set — `setExportKey` has no callers in the tree) to
//!
//! ```text
//! HkdfSha256(master, "TeleForge-Sync-v1", "account")
//!   = SHA-256("TeleForge-Sync-v1" + userId + phone + "account")
//! ```
//!
//! That is one SHA-256 — the function named `HkdfSha256` never calls HKDF, has no
//! extract/expand steps, no salt, no iteration count and no secret input. The
//! key is therefore a *pure function of two public, enumerable values*, and it is
//! the same value that `PgTenantId` derives (`SHA-256(userId:phone)`) and that
//! blinds `chat_id_h` / `account_id_h` / `message_id_h` via `HmacId`. Concrete
//! consequences:
//!
//! 1. A phone number is a low-entropy, enumerable identifier. Anyone holding a
//!    sync-channel dump or a PostgreSQL dump can brute-force the pair offline.
//! 2. The same string yields the same key on every device forever, so there is
//!    no per-device or per-tenant separation and no way to rotate.
//! 3. Because the blinding key equals the encryption key, "opaque" HMAC ids are
//!    computable for any id the attacker guesses, which defeats the point.
//! 4. `ProjectEmbedding` is a *signed permutation* — an orthogonal transform that
//!    preserves cosine and L2 (`teleforge_sync_crypto.cpp` says so explicitly),
//!    so "encrypted" embeddings stay searchable by whoever derived the key.
//!
//! # The scheme used here
//!
//! ```text
//! KEK  = Argon2id(passphrase, salt_32, m_cost, t_cost, p_cost)     // 32 bytes
//! DEK  = random 32 bytes, generated once per tenant
//! wrap = XChaCha20-Poly1305(KEK, aad = tenant_id || key_id, DEK)
//! sub  = HKDF-SHA256(ikm = DEK, salt = "tf/agent/v1/subkeys", info = label)
//! ```
//!
//! Properties that answer the findings directly:
//!
//! * **The salt is random and stored, not derived.** Two tenants with the same
//!   passphrase get different keys, and no offline table can be precomputed.
//! * **Argon2id with a real cost.** `m_cost_kib` is validated to a floor, so
//!   guessing a passphrase costs memory-and-time per attempt, not one SHA-256.
//! * **The DEK is random, not derived.** Passphrase change re-wraps 32 bytes; it
//!   does not re-encrypt the data and does not require the old plaintext.
//! * **Labels are separated.** The id-blinding key, the sync-data key, the
//!   at-rest key and the API token key are different HKDF outputs, so an
//!   attacker who learns one cannot use it for the others (fixes the `HmacId`
//!   reuse).
//! * **`key_id` is versioned.** Rotation is a re-wrap plus a `key_id` bump; the
//!   reader dispatches on the id, so old blobs stay readable.
//!
//! # What is deliberately NOT done
//!
//! * No key is derived from the Telegram user id, phone number, username or any
//!   other server-visible value.
//! * No pepper compiled into the binary (a shipped pepper is not a secret; see
//!   the "why no embedded pepper" note in `docs/architecture.md`).

use crate::crypto::aead::AeadAlgorithm;
use crate::crypto::sig;
use crate::error::{Error, Result};
use crate::secret::SecretBytes;

pub const SALT_LEN: usize = 32;
pub const KEY_LEN: usize = 32;
pub const MIN_PASSPHRASE_CHARS: usize = 12;

/// Cost parameters live with the tenant record so they can be raised without
/// invalidating existing tenants.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct KdfCost {
    pub m_cost_kib: u32,
    pub t_cost: u32,
    pub p_cost: u32,
}

impl Default for KdfCost {
    fn default() -> Self {
        // 64 MiB / 3 passes. Chosen against a *server* budget: the agent may run
        // inside a 512 MiB VPS along with TDLib and SQLite, so 256 MiB (the
        // client default in `docs/architecture.md`) would OOM the box.
        Self {
            m_cost_kib: 65_536,
            t_cost: 3,
            p_cost: 4,
        }
    }
}

impl KdfCost {
    /// Floors prevent a downgraded record from turning Argon2id back into a
    /// cheap hash. Raising the values is always allowed.
    pub const MIN_M_COST_KIB: u32 = 32_768;
    pub const MIN_T_COST: u32 = 2;

    /// The cheapest parameters that pass [`Self::validate`]. Used by `self-test`,
    /// which must prove Argon2id works without spending a second on it.
    pub fn floor() -> Self {
        Self {
            m_cost_kib: Self::MIN_M_COST_KIB,
            t_cost: Self::MIN_T_COST,
            p_cost: 1,
        }
    }

    pub fn validate(&self) -> Result<()> {
        if self.m_cost_kib < Self::MIN_M_COST_KIB {
            return Err(Error::Crypto(format!(
                "kdf m_cost_kib={} below the minimum {}",
                self.m_cost_kib,
                Self::MIN_M_COST_KIB
            )));
        }
        if self.t_cost < Self::MIN_T_COST {
            return Err(Error::Crypto(format!(
                "kdf t_cost={} below the minimum {}",
                self.t_cost,
                Self::MIN_T_COST
            )));
        }
        if self.p_cost == 0 {
            return Err(Error::Crypto("kdf p_cost must be >= 1".to_string()));
        }
        Ok(())
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum KdfAlgorithm {
    /// Argon2id v1.3 (`argon2` crate, `Algorithm::Argon2id`, `Version::V0x13`).
    Argon2idV13,
}

impl KdfAlgorithm {
    pub fn id(self) -> u16 {
        match self {
            Self::Argon2idV13 => 1,
        }
    }

    pub fn from_id(id: u16) -> Option<Self> {
        match id {
            1 => Some(Self::Argon2idV13),
            _ => None,
        }
    }
}

/// Everything needed to re-derive the KEK from a passphrase, except the
/// passphrase. Per tenant, never shared.
#[derive(Clone, Debug)]
pub struct KdfParams {
    pub algorithm: KdfAlgorithm,
    pub cost: KdfCost,
    pub salt: [u8; SALT_LEN],
}

impl KdfParams {
    pub fn new(cost: KdfCost) -> Result<Self> {
        let mut salt = [0u8; SALT_LEN];
        fill_random(&mut salt)?;
        Ok(Self {
            algorithm: KdfAlgorithm::Argon2idV13,
            cost,
            salt,
        })
    }

    pub fn aead(&self) -> AeadAlgorithm {
        // The KEK only ever wraps the DEK, so a 24-byte random nonce is safe for
        // an unbounded number of wraps.
        AeadAlgorithm::XChaCha20Poly1305
    }

    /// AAD binds the wrapped DEK to this tenant and key version, so a swapped
    /// `keys/` file from another tenant fails to unwrap instead of silently
    /// substituting a key.
    pub fn wrap_aad(tenant_id: &[u8; 16], key_id: u16) -> Vec<u8> {
        let mut aad = Vec::with_capacity(24);
        aad.extend_from_slice(b"tf/agent/wrap\0");
        aad.extend_from_slice(tenant_id);
        aad.extend_from_slice(&key_id.to_be_bytes());
        aad
    }
}

/// Reject passphrases that cannot carry enough entropy. Length is a weak proxy,
/// but it catches the realistic failure ("user types `1234`") without a
/// dictionary dependency.
pub fn validate_passphrase(passphrase: &SecretBytes) -> Result<()> {
    let len = passphrase.expose().len();
    if len < MIN_PASSPHRASE_CHARS {
        return Err(Error::Invalid(format!(
            "passphrase must be at least {MIN_PASSPHRASE_CHARS} bytes, got {len}"
        )));
    }
    Ok(())
}

/// Argon2id → 32-byte KEK.
pub fn derive_kek(passphrase: &SecretBytes, params: &KdfParams) -> Result<SecretBytes> {
    params.cost.validate()?;
    validate_passphrase(passphrase)?;
    derive_kek_with(passphrase, &params.salt, params.cost)
}

/// The raw derivation, shared by the passphrase path and the recovery path.
///
/// The output goes straight into a [`SecretBytes`] and the temporary stack array
/// is wiped with `zeroize` before returning, so there is no unprotected copy of
/// the KEK left behind.
pub fn derive_kek_with(
    passphrase: &SecretBytes,
    salt: &[u8],
    cost: KdfCost,
) -> Result<SecretBytes> {
    cost.validate()?;
    if salt.len() != SALT_LEN {
        return Err(Error::Crypto(format!(
            "kdf salt must be {SALT_LEN} bytes, got {}",
            salt.len()
        )));
    }
    if passphrase.is_empty() {
        return Err(Error::Invalid("passphrase must not be empty".to_string()));
    }
    use argon2::{Algorithm, Argon2, Params, Version};
    use zeroize::Zeroize;

    let params = Params::new(cost.m_cost_kib, cost.t_cost, cost.p_cost, Some(KEY_LEN))
        .map_err(|error| Error::Crypto(format!("argon2 params rejected: {error}")))?;
    let argon = Argon2::new(Algorithm::Argon2id, Version::V0x13, params);
    let mut out = [0u8; KEY_LEN];
    argon
        .hash_password_into(passphrase.expose(), salt, &mut out)
        .map_err(|error| Error::Crypto(format!("argon2id failed: {error}")))?;
    let kek = SecretBytes::new(out.to_vec());
    out.zeroize();
    Ok(kek)
}

/// HKDF-SHA256, extract + expand. Unlike the C++ `HkdfSha256`, this really is
/// HKDF: a salted extract step and a labelled expand step.
pub fn hkdf_sha256(
    ikm: &SecretBytes,
    salt: &[u8],
    info: &[u8],
    out_len: usize,
) -> Result<SecretBytes> {
    if out_len == 0 || out_len > 8160 {
        return Err(Error::Crypto(format!("bad hkdf out_len {out_len}")));
    }
    use hkdf::Hkdf;
    use sha2::Sha256;

    let hkdf = Hkdf::<Sha256>::new(Some(salt), ikm.expose());
    let mut out = vec![0u8; out_len];
    hkdf.expand(info, &mut out)
        .map_err(|error| Error::Crypto(format!("hkdf expand failed: {error}")))?;
    Ok(SecretBytes::new(out))
}

/// Domain separation labels. Adding a label is a format change; changing the
/// meaning of an existing one is a data-loss bug.
pub const LABEL_AT_REST: &[u8] = b"tf/agent/v1/at-rest";
pub const LABEL_TDLIB_DB: &[u8] = b"tf/agent/v1/tdlib-db";
pub const LABEL_SYNC_DATA: &[u8] = b"tf/agent/v1/sync-data";
pub const LABEL_BLIND: &[u8] = b"tf/agent/v1/blind";
pub const LABEL_AUTH: &[u8] = b"tf/agent/v1/auth";
pub const LABEL_VERIFIER: &[u8] = b"tf/agent/v1/verifier";
pub const LABEL_RECOVERY: &[u8] = b"tf/agent/v1/recovery";
/// The connection string of the user's PostgreSQL/MySQL, sealed at rest
/// (`docs/db-ingest.md` § 2). Its own label, not `LABEL_AT_REST`: a ciphertext
/// sealed under one label must not open under the other, otherwise a value could
/// be moved between the two slots.
pub const LABEL_DB_CONN: &[u8] = b"tf/agent/v1/db-conn";

const SUBKEY_SALT: &[u8] = b"tf/agent/v1/subkeys";

/// Derive a labelled subkey from the tenant DEK.
pub fn subkey(dek: &SecretBytes, label: &[u8]) -> Result<SecretBytes> {
    if dek.len() != KEY_LEN {
        return Err(Error::Crypto(format!(
            "DEK must be {KEY_LEN} bytes, got {}",
            dek.len()
        )));
    }
    hkdf_sha256(dek, SUBKEY_SALT, label, KEY_LEN)
}

/// Unlock verifier.
///
/// Not a hash of the KEK. It is a fixed plaintext sealed under the KEK, so a
/// wrong passphrase fails the AEAD tag instead of matching a stored digest that
/// an offline attacker could compare against. (`verifier = SHA-256(KEK)` would
/// hand the attacker a one-step oracle per guess on top of Argon2id.)
pub fn verifier_plaintext() -> &'static [u8] {
    b"teleforge-agent-unlock-verifier-v1"
}

/// 16-byte recovery code, printed once at `pair`/init time. Its own Argon2id
/// pass derives a *second* wrapping key so losing the passphrase does not lose
/// the sessions.
pub fn derive_recovery_kek(code: &SecretBytes, params: &KdfParams) -> Result<SecretBytes> {
    params.cost.validate()?;
    if code.expose().len() < 16 {
        return Err(Error::Invalid("recovery code too short".to_string()));
    }
    // The recovery code must not derive the *same* KEK as a passphrase with the
    // same bytes, and it must not be openable by the passphrase Argon2 output.
    // Mixing a distinct label into the salt gives a different Argon2 input, so
    // the two derivations are separated even if the code and the passphrase were
    // identical.
    use sha2::{Digest, Sha256};

    let mut hasher = Sha256::new();
    hasher.update(params.salt);
    hasher.update(LABEL_RECOVERY);
    let mut salt = [0u8; SALT_LEN];
    salt.copy_from_slice(&hasher.finalize());
    derive_kek_with(code, &salt, params.cost)
}

fn fill_random(buf: &mut [u8]) -> Result<()> {
    use rand::RngCore;
    rand::rngs::OsRng.fill_bytes(buf);
    Ok(())
}

/// Re-export so callers that only need a hash of a non-secret can find it here.
pub use sig::sha256_hex_or_stub as sha256_hex;

#[cfg(test)]
mod tests {
    use super::*;

    /// Floors, not the defaults: the tests run Argon2id ~10 times and 64 MiB x 3
    /// passes each would make `cargo test` slow without testing anything new.
    fn test_cost() -> KdfCost {
        KdfCost {
            m_cost_kib: KdfCost::MIN_M_COST_KIB,
            t_cost: KdfCost::MIN_T_COST,
            p_cost: 1,
        }
    }

    fn passphrase(text: &str) -> SecretBytes {
        SecretBytes::new(text.as_bytes().to_vec())
    }

    #[test]
    fn kek_is_deterministic_for_the_same_salt_and_cost() {
        let params = KdfParams {
            algorithm: KdfAlgorithm::Argon2idV13,
            cost: test_cost(),
            salt: [7u8; SALT_LEN],
        };
        let a = derive_kek(&passphrase("correct horse battery"), &params).unwrap();
        let b = derive_kek(&passphrase("correct horse battery"), &params).unwrap();
        assert_eq!(a.len(), KEY_LEN);
        assert_eq!(a.expose(), b.expose());
    }

    #[test]
    fn kek_changes_with_salt_and_passphrase() {
        let base = KdfParams {
            algorithm: KdfAlgorithm::Argon2idV13,
            cost: test_cost(),
            salt: [1u8; SALT_LEN],
        };
        let a = derive_kek(&passphrase("correct horse battery"), &base).unwrap();
        let mut other_salt = base.clone();
        other_salt.salt = [2u8; SALT_LEN];
        let b = derive_kek(&passphrase("correct horse battery"), &other_salt).unwrap();
        let c = derive_kek(&passphrase("correct horse batterz"), &base).unwrap();
        assert_ne!(a.expose(), b.expose(), "salt must feed the derivation");
        assert_ne!(
            a.expose(),
            c.expose(),
            "passphrase must feed the derivation"
        );
    }

    #[test]
    fn short_passphrase_and_low_cost_are_rejected() {
        let params = KdfParams {
            algorithm: KdfAlgorithm::Argon2idV13,
            cost: test_cost(),
            salt: [3u8; SALT_LEN],
        };
        assert!(derive_kek(&passphrase("short"), &params).is_err());

        let downgraded = KdfCost {
            m_cost_kib: 1,
            t_cost: 1,
            p_cost: 1,
        };
        assert!(downgraded.validate().is_err());
        assert!(derive_kek_with(
            &passphrase("long enough passphrase"),
            &[0u8; 32],
            downgraded
        )
        .is_err());
    }

    #[test]
    fn salt_length_is_enforced() {
        let error = derive_kek_with(
            &passphrase("long enough passphrase"),
            &[0u8; 8],
            test_cost(),
        );
        assert!(error.is_err());
    }

    #[test]
    fn new_params_have_a_random_salt() {
        let a = KdfParams::new(test_cost()).unwrap();
        let b = KdfParams::new(test_cost()).unwrap();
        assert_eq!(a.salt.len(), SALT_LEN);
        assert_ne!(a.salt, b.salt);
        assert!(a.salt.iter().any(|byte| *byte != 0));
    }

    #[test]
    fn hkdf_labels_are_separated() {
        let ikm = SecretBytes::new(vec![4u8; KEY_LEN]);
        let a = subkey(&ikm, LABEL_SYNC_DATA).unwrap();
        let b = subkey(&ikm, LABEL_BLIND).unwrap();
        let c = subkey(&ikm, LABEL_SYNC_DATA).unwrap();
        assert_eq!(a.len(), KEY_LEN);
        assert_eq!(a.expose(), c.expose(), "subkey must be deterministic");
        assert_ne!(a.expose(), b.expose(), "labels must not collide");
    }

    #[test]
    fn hkdf_rejects_impossible_output_lengths() {
        let ikm = SecretBytes::new(vec![1u8; KEY_LEN]);
        assert!(hkdf_sha256(&ikm, b"salt", b"info", 0).is_err());
        assert!(hkdf_sha256(&ikm, b"salt", b"info", 9000).is_err());
        assert!(hkdf_sha256(&ikm, b"salt", b"info", 32).is_ok());
    }

    #[test]
    fn subkey_rejects_a_wrong_sized_dek() {
        let ikm = SecretBytes::new(vec![1u8; 16]);
        assert!(subkey(&ikm, LABEL_AT_REST).is_err());
    }

    #[test]
    fn recovery_derivation_differs_from_the_passphrase_path() {
        let params = KdfParams {
            algorithm: KdfAlgorithm::Argon2idV13,
            cost: test_cost(),
            salt: [5u8; SALT_LEN],
        };
        // Same bytes on purpose: the label, not the secret, must separate them.
        let secret = passphrase("recoverable-code-1234");
        let via_passphrase = derive_kek(&secret, &params).unwrap();
        let via_recovery = derive_recovery_kek(&secret, &params).unwrap();
        assert_ne!(via_passphrase.expose(), via_recovery.expose());
    }

    #[test]
    fn verifier_plaintext_is_not_a_hash_of_the_kek() {
        let params = KdfParams {
            algorithm: KdfAlgorithm::Argon2idV13,
            cost: test_cost(),
            salt: [6u8; SALT_LEN],
        };
        let kek = derive_kek(&passphrase("correct horse battery"), &params).unwrap();
        let verifier = verifier_plaintext();
        assert_ne!(verifier, kek.expose());
        assert_eq!(verifier.len(), 34);
        assert!(verifier.starts_with(b"teleforge-agent-unlock"));
    }
}
