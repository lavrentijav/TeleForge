//! The vault slot for the DB connection string.
//!
//! # Where the password lives (finding M14)
//!
//! In the C++ tree the connection string sat in the same SQLite file as the
//! archive, with `password=…` inline (`pgConnString`). The rule now is
//! `docs/transport.md` § 2.6 п. 5: `agent.toml` holds host/port/user/dbname/sslmode,
//! the secret part lives in the vault, and the agent never returns it — not through
//! `status`, not through `db.status`, not through an error message.
//!
//! # The slot
//!
//! ```text
//! <data_dir>/tenants/<tenant>/db/credentials.enc      (0600, parent 0700)
//!     = AEAD(subkey(DEK, LABEL_DB_CONN), aad = "tf/agent/v1/db-conn\0" || tenant, url)
//! ```
//!
//! Its own label, not `LABEL_AT_REST`: the label is the domain separation that stops
//! a ciphertext from being moved from the TDLib-login slot into this one (or back)
//! and being accepted. The AAD additionally binds the file to one tenant, so a
//! `credentials.enc` copied into another tenant's directory does not open.
//!
//! # Locked is a state, not a failure
//!
//! [`load`] needs the tenant DEK, so a locked vault cannot reach the DB at all —
//! which is the correct coupling: the ingest would have nothing to seal the rows
//! with anyway (§ 1.3). The ingest reports [`crate::db::class::VAULT_LOCKED`] and
//! `sync.pull` keeps serving the ciphertext that is already in the journal.
//!
//! # There is no CLI for this yet
//!
//! `teleforge-agent db-credentials set` is the planned surface (docs § 12 of
//! `docs/transport.md`); until it exists the slot is written by the installer over
//! the same SSH channel `unlock` uses, through [`store`]. `docs/db-ingest.md` § 2
//! documents the layout so an operator can check it by hand.

use std::path::Path;

use crate::crypto::aead::{self, AeadAlgorithm};
use crate::crypto::kdf;
use crate::error::{Error, Result};
use crate::fsperm;
use crate::secret::SecretBytes;

/// File name inside `<data_dir>/tenants/<tenant>/db/`.
pub const FILE_NAME: &str = "credentials.enc";

/// The one algorithm this slot is written with. Fixed rather than configurable: the
/// file format is the agent's own, and a configurable algorithm here would be a
/// second, less reviewed knob next to `AgentConfig::at_rest_aead`.
pub const ALGORITHM: AeadAlgorithm = AeadAlgorithm::XChaCha20Poly1305;

/// `<data_dir>/tenants/<tenant_hex>/db/credentials.enc`.
///
/// Deliberately *not* validated here: the tenant id is validated by
/// [`crate::vault::sync_db_path`]'s callers and by the store's own path check, and a
/// path helper that quietly rewrites its input would be worse than one that builds
/// the documented path.
pub fn path(data_dir: &str, tenant_hex: &str) -> String {
    format!("{data_dir}/tenants/{tenant_hex}/db/{FILE_NAME}")
}

/// AAD that binds the ciphertext to one tenant.
fn aad(tenant_hex: &str) -> Vec<u8> {
    let mut out = Vec::with_capacity(20 + tenant_hex.len());
    out.extend_from_slice(b"tf/agent/v1/db-conn\0");
    out.extend_from_slice(tenant_hex.as_bytes());
    out
}

/// The sealing key: `subkey(DEK, LABEL_DB_CONN)`.
///
/// Public so the ingest can derive it once at unlock time and never hold the DEK
/// itself (see `db::ingest::KeyMaterial`).
pub fn sealing_key(tenant_dek: &SecretBytes) -> Result<SecretBytes> {
    kdf::subkey(tenant_dek, kdf::LABEL_DB_CONN)
}

/// Write (or replace) the connection string for `tenant_hex`.
///
/// Returns the path, so a caller can report *where* the slot is without reporting
/// what is in it.
pub fn store(
    data_dir: &str,
    tenant_hex: &str,
    tenant_dek: &SecretBytes,
    key_id: u16,
    connection_string: &SecretBytes,
) -> Result<String> {
    if connection_string.is_empty() {
        return Err(Error::Invalid(
            "the DB connection string must not be empty".to_string(),
        ));
    }
    let key = sealing_key(tenant_dek)?;
    let sealed = aead::seal(
        &key,
        key_id,
        ALGORITHM,
        &aad(tenant_hex),
        connection_string.expose(),
    )?;
    let path = path(data_dir, tenant_hex);
    let parent = Path::new(&path)
        .parent()
        .map(|parent| parent.to_string_lossy().to_string())
        .ok_or_else(|| Error::Invalid("the vault path has no parent directory".to_string()))?;
    fsperm::create_private_dir(&parent)?;
    fsperm::write_private_file_replace(&path, &sealed)?;
    Ok(path)
}

/// Read the connection string, or `None` when this tenant has no slot yet.
///
/// `Ok(None)` is a *configuration* state (`db.status.last_error =
/// "credentials_missing"`), not an error: the agent may be installed before the DB
/// access is configured. A slot that exists but does not open is an error, because
/// that means the DEK is wrong or the file was tampered with — and pretending it is
/// "missing" would hide a real event.
pub fn load(
    data_dir: &str,
    tenant_hex: &str,
    tenant_dek: &SecretBytes,
) -> Result<Option<SecretBytes>> {
    let key = sealing_key(tenant_dek)?;
    load_with_key(data_dir, tenant_hex, &key)
}

/// [`load`] with the subkey already derived.
///
/// This is what the ingest uses: it keeps `subkey(DEK, LABEL_DB_CONN)` and never the
/// DEK, so a memory dump of the ingest state does not yield the tenant key.
pub fn load_with_key(
    data_dir: &str,
    tenant_hex: &str,
    conn_key: &SecretBytes,
) -> Result<Option<SecretBytes>> {
    let path = path(data_dir, tenant_hex);
    if !Path::new(&path).exists() {
        return Ok(None);
    }
    let sealed = std::fs::read(&path)?;
    Ok(Some(aead::open(conn_key, &aad(tenant_hex), &sealed)?))
}

/// Remove the slot. Idempotent: removing a slot that is not there is `Ok(false)`.
pub fn remove(data_dir: &str, tenant_hex: &str) -> Result<bool> {
    let path = path(data_dir, tenant_hex);
    if !Path::new(&path).exists() {
        return Ok(false);
    }
    std::fs::remove_file(&path)?;
    Ok(true)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::test_support::TestDir;

    const TENANT: &str = "00112233445566778899aabbccddeeff";

    fn dek() -> SecretBytes {
        // Not a real DEK: the slot only needs a 32-byte key, and a test that runs
        // Argon2id to get one would test Argon2id, not this file.
        SecretBytes::new(vec![0x5a; 32])
    }

    fn conn() -> SecretBytes {
        SecretBytes::new(b"postgresql://teleforge_agent:hunter2@db:5432/tf".to_vec())
    }

    #[test]
    fn a_connection_string_roundtrips_through_the_vault_slot() {
        let directory = TestDir::new("dbcred");
        let data_dir = directory.path().to_string_lossy().to_string();
        let written = store(&data_dir, TENANT, &dek(), 1, &conn()).unwrap();
        assert_eq!(written, path(&data_dir, TENANT));
        assert!(Path::new(&written).exists());

        let read_back = load(&data_dir, TENANT, &dek()).unwrap().unwrap();
        assert_eq!(read_back.expose(), conn().expose());
    }

    #[test]
    fn the_file_on_disk_is_not_the_connection_string() {
        let directory = TestDir::new("dbcred");
        let data_dir = directory.path().to_string_lossy().to_string();
        let written = store(&data_dir, TENANT, &dek(), 1, &conn()).unwrap();
        let bytes = std::fs::read(&written).unwrap();
        let text = String::from_utf8_lossy(&bytes);
        assert!(
            !text.contains("hunter2"),
            "the password is on disk in clear"
        );
        assert!(
            !text.contains("postgresql://"),
            "the URL is on disk in clear"
        );
        // It is the AEAD envelope, not a bare blob.
        assert!(bytes.starts_with(&aead::MAGIC));
    }

    #[test]
    fn an_absent_slot_reads_as_none() {
        let directory = TestDir::new("dbcred");
        let data_dir = directory.path().to_string_lossy().to_string();
        assert!(load(&data_dir, TENANT, &dek()).unwrap().is_none());
        assert!(!remove(&data_dir, TENANT).unwrap());
    }

    #[test]
    fn the_wrong_dek_does_not_open_the_slot() {
        let directory = TestDir::new("dbcred");
        let data_dir = directory.path().to_string_lossy().to_string();
        store(&data_dir, TENANT, &dek(), 1, &conn()).unwrap();
        let other = SecretBytes::new(vec![0x5b; 32]);
        let error = load(&data_dir, TENANT, &other).unwrap_err();
        // The AEAD message never distinguishes "wrong key" from "tampered", and it
        // never contains the plaintext.
        assert!(!error.to_string().contains("hunter2"));
    }

    #[test]
    fn a_slot_moved_to_another_tenant_does_not_open() {
        // AAD binds the file to its tenant directory; copying it must not work.
        let directory = TestDir::new("dbcred");
        let data_dir = directory.path().to_string_lossy().to_string();
        let source = store(&data_dir, TENANT, &dek(), 1, &conn()).unwrap();
        let other_tenant = "ffeeddccbbaa99887766554433221100";
        let target = path(&data_dir, other_tenant);
        let parent = Path::new(&target).parent().unwrap();
        fsperm::create_private_dir(&parent.to_string_lossy()).unwrap();
        std::fs::copy(&source, &target).unwrap();
        assert!(load(&data_dir, other_tenant, &dek()).is_err());
    }

    #[test]
    fn a_slot_sealed_for_one_label_does_not_open_under_another() {
        // The label is the domain separation: a value sealed with LABEL_AT_REST must
        // not be readable through this slot, and vice versa.
        let dek = dek();
        let at_rest = kdf::subkey(&dek, kdf::LABEL_AT_REST).unwrap();
        let sealed = aead::seal(
            &at_rest,
            1,
            ALGORITHM,
            &aad(TENANT),
            b"postgres://wrong/slot",
        )
        .unwrap();
        let db_key = sealing_key(&dek).unwrap();
        assert!(aead::open(&db_key, &aad(TENANT), &sealed).is_err());
    }

    #[test]
    fn an_empty_connection_string_is_refused() {
        let directory = TestDir::new("dbcred");
        let data_dir = directory.path().to_string_lossy().to_string();
        let empty = SecretBytes::new(Vec::new());
        assert!(store(&data_dir, TENANT, &dek(), 1, &empty).is_err());
    }

    #[test]
    fn replacing_the_slot_is_atomic_and_leaves_no_old_copy() {
        let directory = TestDir::new("dbcred");
        let data_dir = directory.path().to_string_lossy().to_string();
        store(&data_dir, TENANT, &dek(), 1, &conn()).unwrap();
        let second = SecretBytes::new(b"mysql://agent:pw@127.0.0.1/tf".to_vec());
        store(&data_dir, TENANT, &dek(), 1, &second).unwrap();
        let read_back = load(&data_dir, TENANT, &dek()).unwrap().unwrap();
        assert_eq!(read_back.expose(), second.expose());
        assert!(remove(&data_dir, TENANT).unwrap());
        assert!(load(&data_dir, TENANT, &dek()).unwrap().is_none());
    }

    #[cfg(unix)]
    #[test]
    fn the_slot_is_private() {
        use std::os::unix::fs::PermissionsExt;
        let directory = TestDir::new("dbcred");
        let data_dir = directory.path().to_string_lossy().to_string();
        let written = store(&data_dir, TENANT, &dek(), 1, &conn()).unwrap();
        let mode = std::fs::metadata(&written).unwrap().permissions().mode() & 0o777;
        assert_eq!(mode, 0o600, "the slot must be 0600, got {mode:o}");
    }
}
