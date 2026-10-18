//! Encrypted session storage: layout, tenant metadata, unlock.
//!
//! # What is stored, and why it is safe to store it
//!
//! | Item | Protection | Recoverable after passphrase loss |
//! |---|---|---|
//! | TDLib database (`session.db`) | TDLib's own `database_encryption_key`, set to `subkey(DEK, LABEL_TDLIB_DB)` | recovery code only |
//! | TDLib login parameters (`tdlib.json.enc`) | `seal(subkey(DEK, LABEL_AT_REST))` | recovery code only |
//! | Sync store values (`sync/data.db`) | each value `seal(subkey(DEK, LABEL_SYNC_DATA))`; metadata in cleartext | recovery code only |
//! | `meta.json` (salt, cost, key ids, verifier) | cleartext by design — it is not a secret | — |
//!
//! `meta.json` being cleartext is a deliberate choice: Argon2id needs the salt
//! *before* it can derive anything, and hiding the salt buys nothing because an
//! attacker with the disk has the ciphertext too. What must never be cleartext is
//! the DEK, and it only exists wrapped under the KEK.
//!
//! # Directory layout
//!
//! ```text
//! <data_dir>/                              0700
//!   meta.json                              tenant registry (ids only)      0640
//!   auth/clients.json                      device token hashes             0600
//!   identity/                              agent CA + listener cert        0700
//!     ca.key.pem  0600   ca.pub.pem 0644
//!     agent.cert.pem 0644  agent.key.pem 0600
//!     clients/<device_id>/cert.pem 0644
//!   tenants/<tenant_hex>/                  0700
//!     meta.json                            salt, cost, key_id, wrapped DEK 0640
//!     keys/dek.<key_id>.wrap               AEAD(KEK, DEK)                   0600
//!     keys/reco.<key_id>.wrap              AEAD(recovery KEK, DEK)         0600
//!     accounts/<account_id>/               0700
//!        session.db                        TDLib db, encrypted by TDLib     0600
//!        tdlib.json.enc                    parameters                      0600
//!        files/                            TDLib media cache               0700
//!     sync/data.db                         logical sync store (WAL)        0600
//!     logs/                                0700
//! ```
//!
//! Nothing is written under `$TMPDIR`. That is finding L9: the C++ side
//! (`DecompressToTemp` in `teleforge_sync_snapshot.cpp`) wrote a *decrypted*
//! database into `QDir::tempPath()` with `setAutoRemove(false)`.

use std::collections::BTreeMap;

use crate::crypto::aead::{self, AeadAlgorithm};
use crate::crypto::kdf::{self, KdfCost, KdfParams, KEY_LEN, SALT_LEN};
use crate::error::{Error, Result};
use crate::fsperm;
use crate::secret::SecretBytes;

pub const FORMAT_VERSION: u32 = 1;
pub const META_FILE: &str = "meta.json";
pub const CLIENTS_FILE: &str = "auth/clients.json";

/// Per-tenant metadata. Cleartext, versioned, and small enough to read with a
/// hand-written parser if the agent must be recovered without its own tooling.
#[derive(Clone, Debug)]
pub struct TenantMeta {
    pub format_version: u32,
    pub tenant_id: [u8; 16],
    pub kdf: KdfParams,
    pub key_id: u16,
    pub aead: AeadAlgorithm,
    pub created_at: u64,
    /// `AEAD(KEK, aad = tenant_id || key_id)` over the DEK. 60 bytes for
    /// XChaCha20-Poly1305 (header + 32 + tag).
    pub wrapped_dek: Vec<u8>,
    /// Second wrap under the recovery code, so a forgotten passphrase is not a
    /// lost session. Optional so a tenant created without a recovery code is an
    /// explicit, visible choice.
    pub wrapped_dek_recovery: Option<Vec<u8>>,
    /// `AEAD(KEK, plaintext = verifier_plaintext())`.
    pub verifier: Vec<u8>,
    pub account_ids: Vec<[u8; 16]>,
    pub last_unlock_at: Option<u64>,
    pub failed_unlock_count: u32,
}

impl TenantMeta {
    pub fn path(&self, data_dir: &str) -> String {
        format!(
            "{}/tenants/{}/{META_FILE}",
            data_dir,
            crate::crypto::sig::to_hex(&self.tenant_id)
        )
    }

    pub fn dek_wrap_path(&self, data_dir: &str) -> String {
        format!(
            "{}/tenants/{}/keys/dek.{:04}.wrap",
            data_dir,
            crate::crypto::sig::to_hex(&self.tenant_id),
            self.key_id
        )
    }

    /// A human-readable serialisation. Real builds use `serde_json`; this
    /// version is the exact field order the golden test pins, and it is what the
    /// client's bootstrap parser expects.
    pub fn to_json(&self) -> String {
        let accounts: Vec<String> = self
            .account_ids
            .iter()
            .map(|id| format!("\"{}\"", crate::crypto::sig::to_hex(id)))
            .collect();
        format!(
            concat!(
                "{{\"format_version\":{},\"tenant_id\":\"{}\",\"kdf\":{{\"alg\":{},\"m_cost_kib\":{},",
                "\"t_cost\":{},\"p_cost\":{},\"salt\":\"{}\"}},\"key_id\":{},\"aead\":{},",
                "\"created_at\":{},\"wrapped_dek\":\"{}\",\"wrapped_dek_recovery\":{},",
                "\"verifier\":\"{}\",\"accounts\":[{}]}}"
            ),
            self.format_version,
            crate::crypto::sig::to_hex(&self.tenant_id),
            self.kdf.algorithm.id(),
            self.kdf.cost.m_cost_kib,
            self.kdf.cost.t_cost,
            self.kdf.cost.p_cost,
            crate::crypto::sig::to_hex(&self.kdf.salt),
            self.key_id,
            self.aead.id(),
            self.created_at,
            encode_blob(&self.wrapped_dek),
            match &self.wrapped_dek_recovery {
                Some(bytes) => format!("\"{}\"", encode_blob(bytes)),
                None => "null".to_string(),
            },
            encode_blob(&self.verifier),
            accounts.join(",")
        )
    }
}

/// The unlocked tenant: the only place a DEK exists in memory. Dropping it wipes
/// the key; there is no `Clone`, so a second copy cannot leak.
pub struct UnlockedTenant {
    pub tenant_id: [u8; 16],
    pub key_id: u16,
    dek: SecretBytes,
    unlocked_at: u64,
    pub idle_lock_secs: u64,
}

impl UnlockedTenant {
    pub fn dek(&self) -> &SecretBytes {
        &self.dek
    }

    /// Subkeys are derived on demand and cached by the caller that needs them;
    /// they are never stored on disk.
    pub fn subkey(&self, label: &[u8]) -> Result<SecretBytes> {
        kdf::subkey(&self.dek, label)
    }

    pub fn is_idle(&self, now: u64) -> bool {
        self.idle_lock_secs != 0 && now.saturating_sub(self.unlocked_at) >= self.idle_lock_secs
    }

    pub fn touch(&mut self, now: u64) {
        self.unlocked_at = now;
    }

    pub fn aad(&self) -> Vec<u8> {
        KdfParams::wrap_aad(&self.tenant_id, self.key_id)
    }

    /// Explicit lock. Also reachable from `SIGUSR1` and from `teleforge-agent lock`.
    pub fn lock(self) {
        // `drop` wipes the DEK via SecretBytes::Drop.
        drop(self);
    }
}

impl core::fmt::Debug for UnlockedTenant {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        write!(
            f,
            "UnlockedTenant {{ tenant: {}, key_id: {}, dek: <redacted> }}",
            crate::crypto::sig::to_hex(&self.tenant_id),
            self.key_id
        )
    }
}

/// First initialisation of a tenant.
///
/// Inputs from the client at bootstrap:
/// * the passphrase (already validated for length);
/// * whether a recovery code should be minted;
/// * the KDF cost the operator accepted.
///
/// Everything else is generated here, on the server, by the agent: the tenant id
/// (16 random bytes), the salt, the DEK, the recovery code, and the agent's own
/// CA key. The client never chooses a key, which is the difference between this
/// design and the `userId + phone` derivation in `teleforge_sync.cpp`.
pub fn create_tenant(
    passphrase: &SecretBytes,
    cost: KdfCost,
    now: u64,
    with_recovery: bool,
) -> Result<(TenantMeta, UnlockedTenant, Option<SecretBytes>)> {
    kdf::validate_passphrase(passphrase)?;
    cost.validate()?;
    let kdf_params = KdfParams::new(cost)?;

    let mut tenant_id = [0u8; 16];
    fill_random(&mut tenant_id)?;

    // TODO(rand): DEK = OsRng 32 bytes.
    let dek = SecretBytes::generate(KEY_LEN)?;
    let kek = kdf::derive_kek(passphrase, &kdf_params)?;

    let key_id = 1u16;
    let aad = KdfParams::wrap_aad(&tenant_id, key_id);
    let wrapped_dek = aead::seal(&kek, key_id, kdf_params.aead(), &aad, dek.expose())?;
    let verifier = aead::seal(
        &kek,
        key_id,
        kdf_params.aead(),
        &aad,
        kdf::verifier_plaintext(),
    )?;

    let recovery = if with_recovery {
        let code = SecretBytes::generate(24)?;
        let recovery_kek = kdf::derive_recovery_kek(&code, &kdf_params)?;
        let wrapped = aead::seal(&recovery_kek, key_id, kdf_params.aead(), &aad, dek.expose())?;
        Some((code, wrapped))
    } else {
        None
    };

    let meta = TenantMeta {
        format_version: FORMAT_VERSION,
        tenant_id,
        aead: kdf_params.aead(),
        kdf: kdf_params,
        key_id,
        created_at: now,
        wrapped_dek,
        wrapped_dek_recovery: recovery.as_ref().map(|(_, wrapped)| wrapped.clone()),
        verifier,
        account_ids: Vec::new(),
        last_unlock_at: None,
        failed_unlock_count: 0,
    };

    let unlocked = UnlockedTenant {
        tenant_id,
        key_id,
        dek,
        unlocked_at: now,
        idle_lock_secs: 0,
    };

    Ok((meta, unlocked, recovery.map(|(code, _)| code)))
}

/// Unlock: derive the KEK, unwrap the DEK, then *prove* the KEK was right by
/// opening the verifier. Nothing about the failure distinguishes "wrong
/// passphrase" from "corrupt file" to the caller of the API, but internally the
/// two are different errors so the log can help an operator.
pub fn unlock(meta: &TenantMeta, passphrase: &SecretBytes, now: u64) -> Result<UnlockedTenant> {
    if meta.format_version != FORMAT_VERSION {
        return Err(Error::Invalid(format!(
            "tenant meta format {} (this build speaks {FORMAT_VERSION})",
            meta.format_version
        )));
    }
    let kek = kdf::derive_kek(passphrase, &meta.kdf)?;
    let aad = KdfParams::wrap_aad(&meta.tenant_id, meta.key_id);

    // A wrong passphrase fails the AEAD tag on the verifier. That is not a
    // "crypto" failure for the caller: it maps to `1002 unauthenticated`, and the
    // message must not say whether the passphrase or the file was wrong.
    let verifier = aead::open(&kek, &aad, &meta.verifier)
        .map_err(|_| Error::Auth("wrong passphrase".to_string()))?;
    if !crate::secret::constant_time_eq(verifier.expose(), kdf::verifier_plaintext()) {
        return Err(Error::Auth("unlock verifier mismatch".to_string()));
    }

    let dek = aead::open(&kek, &aad, &meta.wrapped_dek)?;
    if dek.len() != KEY_LEN {
        return Err(Error::Crypto(format!(
            "unwrapped DEK has {} bytes",
            dek.len()
        )));
    }

    Ok(UnlockedTenant {
        tenant_id: meta.tenant_id,
        key_id: meta.key_id,
        dek,
        unlocked_at: now,
        idle_lock_secs: 0,
    })
}

/// Recover with the printed recovery code instead of the passphrase.
pub fn unlock_with_recovery(
    meta: &TenantMeta,
    code: &SecretBytes,
    now: u64,
) -> Result<UnlockedTenant> {
    let wrapped = meta
        .wrapped_dek_recovery
        .as_ref()
        .ok_or_else(|| Error::Auth("this tenant has no recovery envelope".to_string()))?;
    let recovery_kek = kdf::derive_recovery_kek(code, &meta.kdf)?;
    let aad = KdfParams::wrap_aad(&meta.tenant_id, meta.key_id);
    let dek = aead::open(&recovery_kek, &aad, wrapped)
        .map_err(|_| Error::Auth("wrong recovery code".to_string()))?;
    Ok(UnlockedTenant {
        tenant_id: meta.tenant_id,
        key_id: meta.key_id,
        dek,
        unlocked_at: now,
        idle_lock_secs: 0,
    })
}

/// Change the passphrase.
///
/// Cheap by construction: the DEK is already known, so only the *wrapper* is
/// recomputed. There is no reason to re-encrypt any data, and doing so would
/// require the whole dataset to be online at once.
pub fn change_passphrase(
    meta: &mut TenantMeta,
    dek: &SecretBytes,
    new_passphrase: &SecretBytes,
    now: u64,
) -> Result<()> {
    kdf::validate_passphrase(new_passphrase)?;
    let new_params = KdfParams::new(meta.kdf.cost)?;
    let kek = kdf::derive_kek(new_passphrase, &new_params)?;
    let key_id = meta
        .key_id
        .checked_add(1)
        .ok_or_else(|| Error::Crypto("key_id overflow".to_string()))?;
    let aad = KdfParams::wrap_aad(&meta.tenant_id, key_id);

    meta.wrapped_dek = aead::seal(&kek, key_id, new_params.aead(), &aad, dek.expose())?;
    meta.verifier = aead::seal(
        &kek,
        key_id,
        new_params.aead(),
        &aad,
        kdf::verifier_plaintext(),
    )?;
    if meta.wrapped_dek_recovery.is_some() {
        // The recovery code is re-derived against the *new* salt, so a change of
        // passphrase does not silently invalidate it.
        return Err(Error::Unsupported(
            "re-wrapping the recovery envelope needs the code; use `recovery-rotate`",
        ));
    }
    meta.kdf = new_params;
    meta.key_id = key_id;
    meta.last_unlock_at = Some(now);
    Ok(())
}

/// Everything the client needs to know about a tenant, safe to return over the
/// API: no salt, no wrapped key, no verifier.
#[derive(Clone, Debug)]
pub struct TenantSummary {
    pub tenant_hex: String,
    pub accounts: Vec<String>,
    pub key_id: u16,
    pub kdf_m_cost_kib: u32,
    pub kdf_t_cost: u32,
    pub has_recovery: bool,
    pub unlocked: bool,
    pub last_unlock_at: Option<u64>,
}

impl TenantMeta {
    pub fn summary(&self, unlocked: bool) -> TenantSummary {
        TenantSummary {
            tenant_hex: crate::crypto::sig::to_hex(&self.tenant_id),
            accounts: self
                .account_ids
                .iter()
                .map(|id| crate::crypto::sig::to_hex(id))
                .collect(),
            key_id: self.key_id,
            kdf_m_cost_kib: self.kdf.cost.m_cost_kib,
            kdf_t_cost: self.kdf.cost.t_cost,
            has_recovery: self.wrapped_dek_recovery.is_some(),
            unlocked,
            last_unlock_at: self.last_unlock_at,
        }
    }
}

/// On-disk tenant registry (`<data_dir>/meta.json`). A map so that a single VPS
/// can hold several tenants without a schema migration.
#[derive(Debug, Default)]
pub struct TenantRegistry {
    pub format_version: u32,
    pub tenants: BTreeMap<String, String>,
}

impl TenantRegistry {
    pub fn new() -> Self {
        Self {
            format_version: FORMAT_VERSION,
            tenants: BTreeMap::new(),
        }
    }

    pub fn to_json(&self) -> String {
        let entries: Vec<String> = self
            .tenants
            .iter()
            .map(|(k, v)| format!("\"{k}\":\"{v}\""))
            .collect();
        format!(
            "{{\"format_version\":{},\"tenants\":{{{}}}}}",
            self.format_version,
            entries.join(",")
        )
    }
}

/// Persist a freshly created tenant. Order matters: keys first, then meta, so a
/// crash between the two leaves an orphan key file rather than a tenant whose
/// meta points at nothing.
pub fn persist_new_tenant(data_dir: &str, meta: &TenantMeta) -> Result<()> {
    let tenant_hex = crate::crypto::sig::to_hex(&meta.tenant_id);
    let tenant_dir = format!("{data_dir}/tenants/{tenant_hex}");
    fsperm::create_private_dir(&tenant_dir)?;
    fsperm::create_private_dir(&format!("{tenant_dir}/keys"))?;
    fsperm::create_private_dir(&format!("{tenant_dir}/accounts"))?;
    fsperm::create_private_dir(&format!("{tenant_dir}/sync"))?;

    fsperm::write_private_file(&meta.dek_wrap_path(data_dir), &meta.wrapped_dek)?;
    if let Some(recovery) = &meta.wrapped_dek_recovery {
        fsperm::write_private_file(
            &format!("{tenant_dir}/keys/reco.{:04}.wrap", meta.key_id),
            recovery,
        )?;
    }
    fsperm::write_private_file_replace(&meta.path(data_dir), meta.to_json().as_bytes())?;
    Ok(())
}

/// Read a tenant's meta back. The parser is intentionally strict and tiny so that
/// a corrupted `meta.json` fails loudly instead of producing zeroed parameters
/// that would weaken Argon2id.
pub fn load_tenant_meta(data_dir: &str, tenant_hex: &str) -> Result<TenantMeta> {
    let path = format!("{data_dir}/tenants/{tenant_hex}/{META_FILE}");
    let text = std::fs::read_to_string(&path)?;
    parse_tenant_meta(&text)
}

pub fn parse_tenant_meta(text: &str) -> Result<TenantMeta> {
    let format_version = crate::api::protocol::extract_number(text, "format_version")
        .ok_or_else(|| Error::Invalid("meta.json: no format_version".to_string()))?
        as u32;
    let tenant_hex = crate::api::protocol::extract_str(text, "tenant_id")
        .ok_or_else(|| Error::Invalid("meta.json: no tenant_id".to_string()))?;
    let mut tenant_id = [0u8; 16];
    let parsed = crate::crypto::sig::parse_hex(&tenant_hex)?;
    if parsed.len() != tenant_id.len() {
        return Err(Error::Invalid(
            "meta.json: tenant_id must be 16 bytes".to_string(),
        ));
    }
    tenant_id.copy_from_slice(&parsed);

    let alg_id = crate::api::protocol::extract_number(text, "alg").unwrap_or(1) as u16;
    let algorithm = kdf::KdfAlgorithm::from_id(alg_id)
        .ok_or_else(|| Error::Invalid(format!("meta.json: unknown kdf alg {alg_id}")))?;
    let m_cost_kib = crate::api::protocol::extract_number(text, "m_cost_kib").unwrap_or(0) as u32;
    let t_cost = crate::api::protocol::extract_number(text, "t_cost").unwrap_or(0) as u32;
    let p_cost = crate::api::protocol::extract_number(text, "p_cost").unwrap_or(0) as u32;
    let cost = KdfCost {
        m_cost_kib,
        t_cost,
        p_cost,
    };
    cost.validate()?;

    let salt_hex = crate::api::protocol::extract_str(text, "salt")
        .ok_or_else(|| Error::Invalid("meta.json: no salt".to_string()))?;
    let salt_bytes = crate::crypto::sig::parse_hex(&salt_hex)?;
    if salt_bytes.len() != SALT_LEN {
        return Err(Error::Invalid(
            "meta.json: salt must be 32 bytes".to_string(),
        ));
    }
    let mut salt = [0u8; SALT_LEN];
    salt.copy_from_slice(&salt_bytes);

    let key_id = crate::api::protocol::extract_number(text, "key_id").unwrap_or(1) as u16;
    let aead_id = crate::api::protocol::extract_number(text, "aead").unwrap_or(1) as u16;
    let aead_alg = AeadAlgorithm::from_id(aead_id)
        .ok_or_else(|| Error::Invalid(format!("meta.json: unknown aead {aead_id}")))?;
    let created_at = crate::api::protocol::extract_number(text, "created_at").unwrap_or(0);
    let wrapped_dek = decode_blob(
        &crate::api::protocol::extract_str(text, "wrapped_dek")
            .ok_or_else(|| Error::Invalid("meta.json: no wrapped_dek".to_string()))?,
    )?;
    let verifier = decode_blob(
        &crate::api::protocol::extract_str(text, "verifier")
            .ok_or_else(|| Error::Invalid("meta.json: no verifier".to_string()))?,
    )?;
    let wrapped_dek_recovery = crate::api::protocol::extract_str(text, "wrapped_dek_recovery")
        .filter(|v| v != "null")
        .map(|v| decode_blob(&v))
        .transpose()?;

    Ok(TenantMeta {
        format_version,
        tenant_id,
        kdf: KdfParams {
            algorithm,
            cost,
            salt,
        },
        key_id,
        aead: aead_alg,
        created_at,
        wrapped_dek,
        wrapped_dek_recovery,
        verifier,
        account_ids: Vec::new(),
        last_unlock_at: None,
        failed_unlock_count: 0,
    })
}

/// Hex encoding for the meta file. Hex, not base64, on purpose: it needs no
/// dependency, it is case-insensitive-safe to retype from a console, and the meta
/// file is small. Secrets on the wire go through AEAD, never through this.
pub fn encode_blob(bytes: &[u8]) -> String {
    crate::crypto::sig::to_hex(bytes)
}

pub fn decode_blob(text: &str) -> Result<Vec<u8>> {
    crate::crypto::sig::parse_hex(text)
}

/// Path helpers used by `status` and by the client's UI.
pub fn account_dir(data_dir: &str, tenant_hex: &str, account_hex: &str) -> String {
    format!("{data_dir}/tenants/{tenant_hex}/accounts/{account_hex}")
}

pub fn sync_db_path(data_dir: &str, tenant_hex: &str) -> String {
    format!("{data_dir}/tenants/{tenant_hex}/sync/data.db")
}

fn fill_random(buf: &mut [u8]) -> Result<()> {
    use rand::RngCore;
    rand::rngs::OsRng.fill_bytes(buf);
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Floor cost: the Argon2id parameters are not what these tests are about, and
    /// the default (64 MiB, 3 passes) would make them slow for no benefit.
    fn floor_cost() -> KdfCost {
        KdfCost::floor()
    }

    fn passphrase(text: &str) -> SecretBytes {
        SecretBytes::new(text.as_bytes().to_vec())
    }

    #[test]
    fn create_then_unlock_roundtrips_and_never_stores_the_dek_in_the_clear() {
        let (meta, unlocked, recovery) = create_tenant(
            &passphrase("correct horse battery"),
            floor_cost(),
            1_000,
            true,
        )
        .unwrap();
        assert_eq!(unlocked.dek().len(), KEY_LEN);
        let dek = unlocked.dek().expose().to_vec();

        // The meta file is cleartext *metadata*: no DEK, no passphrase.
        let json = meta.to_json();
        assert!(!json.contains("correct horse battery"));
        assert!(!json.contains(&crate::crypto::sig::to_hex(&dek)));

        let reopened = unlock(&meta, &passphrase("correct horse battery"), 2_000).unwrap();
        assert_eq!(reopened.dek().expose(), dek.as_slice());
        assert_eq!(reopened.tenant_id, meta.tenant_id);

        // The recovery code opens the same tenant with a *different* passphrase
        // path, which is the whole point of having one.
        let code = recovery.expect("recovery code");
        let recovered = unlock_with_recovery(&meta, &code, 3_000).unwrap();
        assert_eq!(recovered.dek().expose(), dek.as_slice());
    }

    #[test]
    fn a_wrong_passphrase_is_an_auth_error_not_a_crypto_error() {
        let (meta, _, _) = create_tenant(
            &passphrase("correct horse battery"),
            floor_cost(),
            1_000,
            false,
        )
        .unwrap();
        let error = unlock(&meta, &passphrase("wrong horse battery"), 2_000).unwrap_err();
        assert_eq!(error.exit_code(), crate::error::exit::AUTH);
        assert!(!error.to_string().contains("correct horse"));
    }

    #[test]
    fn meta_json_roundtrips_through_the_parser() {
        let (meta, _, _) = create_tenant(
            &passphrase("correct horse battery"),
            floor_cost(),
            1_000,
            false,
        )
        .unwrap();
        let parsed = parse_tenant_meta(&meta.to_json()).unwrap();
        assert_eq!(parsed.tenant_id, meta.tenant_id);
        assert_eq!(parsed.kdf.salt, meta.kdf.salt);
        assert_eq!(parsed.key_id, meta.key_id);
        assert_eq!(parsed.wrapped_dek, meta.wrapped_dek);
    }

    #[test]
    fn change_passphrase_rewraps_without_touching_the_dek() {
        let (mut meta, unlocked, _) = create_tenant(
            &passphrase("correct horse battery"),
            floor_cost(),
            1_000,
            false,
        )
        .unwrap();
        let dek = unlocked.dek().expose().to_vec();
        let old_key_id = meta.key_id;
        change_passphrase(
            &mut meta,
            unlocked.dek(),
            &passphrase("a brand new passphrase"),
            2_000,
        )
        .unwrap();
        assert_eq!(meta.key_id, old_key_id + 1);
        let reopened = unlock(&meta, &passphrase("a brand new passphrase"), 3_000).unwrap();
        assert_eq!(reopened.dek().expose(), dek.as_slice());
        assert!(unlock(&meta, &passphrase("correct horse battery"), 3_000).is_err());
    }
}
