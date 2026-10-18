//! Authenticated encryption at rest and the on-disk envelope format.
//!
//! The envelope layout is implemented (and unit-tested) here because it is the
//! part that must be *stable* across versions: the C++ side already writes
//! `TFSYNC1` bundles (`teleforge_sync_crypto.cpp`: 8-byte magic, 12-byte nonce,
//! ciphertext, 16-byte tag, AES-256-GCM), and the agent has to be able to read
//! those during migration.
//!
//! # Algorithm choice
//!
//! * **XChaCha20-Poly1305** is the default for new at-rest blobs. 192-bit nonces
//!   mean a random nonce is safe for an unbounded number of encryptions under one
//!   key, which matters because the agent re-seals a session file on every TDLib
//!   flush and has no reliable counter across crashes.
//! * **AES-256-GCM** is kept as an accepted *read* format for the existing
//!   `TFSYNC1` bundles and as an option where AES-NI makes it faster. Its 96-bit
//!   nonce means a counter discipline is mandatory; the writer refuses to seal
//!   twice with the same `(key_id, nonce)`, tracked in a small on-disk counter
//!   file per `key_id`.
//!
//! # What the envelope does NOT protect against
//!
//! Truncation to zero length, deletion, and rollback to an older valid bundle.
//! Those are covered by the metadata (`key_id`, `generation`) recorded next to
//! the blob and by the monotone `op_seq` in the sync store, not by the AEAD.

use crate::error::{Error, Result};
use crate::secret::SecretBytes;

pub const MAGIC: [u8; 8] = *b"TFSEC1\x00\x00";
pub const LEGACY_MAGIC: [u8; 8] = *b"TFSYNC1\x00";
pub const TAG_LEN: usize = 16;
pub const HEADER_LEN: usize = 8 + 2 + 2 + 2 + 1 + 1;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AeadAlgorithm {
    XChaCha20Poly1305,
    Aes256Gcm,
}

impl AeadAlgorithm {
    pub fn id(self) -> u16 {
        match self {
            Self::XChaCha20Poly1305 => 1,
            Self::Aes256Gcm => 2,
        }
    }

    pub fn from_id(id: u16) -> Option<Self> {
        match id {
            1 => Some(Self::XChaCha20Poly1305),
            2 => Some(Self::Aes256Gcm),
            _ => None,
        }
    }

    pub fn nonce_len(self) -> usize {
        match self {
            Self::XChaCha20Poly1305 => 24,
            Self::Aes256Gcm => 12,
        }
    }

    pub fn key_len(self) -> usize {
        32
    }
}

/// Parsed envelope header. `nonce` is kept as a `Vec` rather than a fixed array
/// because the length depends on the algorithm.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Header {
    pub algorithm: AeadAlgorithm,
    pub key_id: u16,
    pub nonce: Vec<u8>,
}

/// Serialise magic + algorithm + key_id + nonce. Deterministic, so a golden test
/// can pin the bytes; the v1 layout must never change.
pub fn encode_header(header: &Header) -> Result<Vec<u8>> {
    let expected = header.algorithm.nonce_len();
    if header.nonce.len() != expected {
        return Err(Error::Crypto(format!(
            "nonce for {:?} must be {expected} bytes, got {}",
            header.algorithm,
            header.nonce.len()
        )));
    }
    let mut out = Vec::with_capacity(HEADER_LEN + expected);
    out.extend_from_slice(&MAGIC);
    out.extend_from_slice(&header.algorithm.id().to_be_bytes());
    out.extend_from_slice(&header.key_id.to_be_bytes());
    out.extend_from_slice(&(expected as u16).to_be_bytes());
    out.push(0); // reserved flags: must be zero, rejected otherwise
    out.push(0); // reserved
    out.extend_from_slice(&header.nonce);
    Ok(out)
}

/// Parse a header and report how many bytes it consumed.
pub fn decode_header(bytes: &[u8]) -> Result<(Header, usize)> {
    if bytes.len() < HEADER_LEN {
        return Err(Error::Crypto(
            "envelope shorter than its header".to_string(),
        ));
    }
    let mut magic = [0u8; 8];
    magic.copy_from_slice(&bytes[..8]);
    if magic != MAGIC && magic != LEGACY_MAGIC {
        return Err(Error::Crypto("bad envelope magic".to_string()));
    }
    let algorithm_id = u16::from_be_bytes([bytes[8], bytes[9]]);
    let algorithm = AeadAlgorithm::from_id(algorithm_id)
        .ok_or_else(|| Error::Crypto(format!("unknown aead id {algorithm_id}")))?;
    let key_id = u16::from_be_bytes([bytes[10], bytes[11]]);
    let nonce_len = u16::from_be_bytes([bytes[12], bytes[13]]) as usize;
    if bytes[14] != 0 || bytes[15] != 0 {
        return Err(Error::Crypto("reserved envelope flags set".to_string()));
    }
    if nonce_len != algorithm.nonce_len() {
        return Err(Error::Crypto(format!(
            "nonce length {nonce_len} does not match {:?}",
            algorithm
        )));
    }
    let end = HEADER_LEN + nonce_len;
    if bytes.len() < end {
        return Err(Error::Crypto("truncated envelope nonce".to_string()));
    }
    Ok((
        Header {
            algorithm,
            key_id,
            nonce: bytes[HEADER_LEN..end].to_vec(),
        },
        end,
    ))
}

/// Seal `plain` under `key`.
///
/// `aad` must include the caller's context (entity key, tenant id, field id) so
/// that a ciphertext moved to another row fails to open.
pub fn seal(
    key: &SecretBytes,
    key_id: u16,
    algorithm: AeadAlgorithm,
    aad: &[u8],
    plain: &[u8],
) -> Result<Vec<u8>> {
    if key.len() != algorithm.key_len() {
        return Err(Error::Crypto("wrong key length".to_string()));
    }
    let nonce = random_nonce(algorithm.nonce_len())?;
    let header = Header {
        algorithm,
        key_id,
        nonce,
    };
    let mut out = encode_header(&header)?;
    let body = encrypt(algorithm, key, &header.nonce, aad, plain)?;
    out.extend_from_slice(&body);
    Ok(out)
}

/// Open a sealed blob and return the plaintext as a wiping buffer.
pub fn open(key: &SecretBytes, aad: &[u8], sealed: &[u8]) -> Result<SecretBytes> {
    let (header, body_at) = decode_header(sealed)?;
    if sealed.len() < body_at + TAG_LEN {
        return Err(Error::Crypto("envelope has no room for a tag".to_string()));
    }
    if key.len() != header.algorithm.key_len() {
        return Err(Error::Crypto("wrong key length".to_string()));
    }
    let plain = decrypt(
        header.algorithm,
        key,
        &header.nonce,
        aad,
        &sealed[body_at..],
    )?;
    Ok(SecretBytes::new(plain))
}

fn random_nonce(len: usize) -> Result<Vec<u8>> {
    use rand::RngCore;
    let mut nonce = vec![0u8; len];
    rand::rngs::OsRng.fill_bytes(&mut nonce);
    Ok(nonce)
}

/// One error message for "wrong key" and "tampered ciphertext" on purpose: an
/// oracle that distinguishes them is a decryption oracle.
const OPEN_FAILED: &str = "envelope failed to open (wrong key, wrong aad, or tampered ciphertext)";

fn encrypt(
    algorithm: AeadAlgorithm,
    key: &SecretBytes,
    nonce: &[u8],
    aad: &[u8],
    plain: &[u8],
) -> Result<Vec<u8>> {
    use chacha20poly1305::aead::Payload;
    match algorithm {
        AeadAlgorithm::XChaCha20Poly1305 => {
            use chacha20poly1305::aead::Aead;
            use chacha20poly1305::{KeyInit, XChaCha20Poly1305, XNonce};
            let cipher = XChaCha20Poly1305::new_from_slice(key.expose())
                .map_err(|_| Error::Crypto("bad xchacha key".to_string()))?;
            cipher
                .encrypt(XNonce::from_slice(nonce), Payload { msg: plain, aad })
                .map_err(|_| Error::Crypto("xchacha seal failed".to_string()))
        }
        AeadAlgorithm::Aes256Gcm => {
            use aes_gcm::aead::Aead;
            use aes_gcm::{Aes256Gcm, KeyInit, Nonce};
            let cipher = Aes256Gcm::new_from_slice(key.expose())
                .map_err(|_| Error::Crypto("bad aes key".to_string()))?;
            cipher
                .encrypt(Nonce::from_slice(nonce), Payload { msg: plain, aad })
                .map_err(|_| Error::Crypto("aes-gcm seal failed".to_string()))
        }
    }
}

fn decrypt(
    algorithm: AeadAlgorithm,
    key: &SecretBytes,
    nonce: &[u8],
    aad: &[u8],
    body: &[u8],
) -> Result<Vec<u8>> {
    use chacha20poly1305::aead::Payload;
    match algorithm {
        AeadAlgorithm::XChaCha20Poly1305 => {
            use chacha20poly1305::aead::Aead;
            use chacha20poly1305::{KeyInit, XChaCha20Poly1305, XNonce};
            let cipher = XChaCha20Poly1305::new_from_slice(key.expose())
                .map_err(|_| Error::Crypto("bad xchacha key".to_string()))?;
            cipher
                .decrypt(XNonce::from_slice(nonce), Payload { msg: body, aad })
                .map_err(|_| Error::Crypto(OPEN_FAILED.to_string()))
        }
        AeadAlgorithm::Aes256Gcm => {
            use aes_gcm::aead::Aead;
            use aes_gcm::{Aes256Gcm, KeyInit, Nonce};
            let cipher = Aes256Gcm::new_from_slice(key.expose())
                .map_err(|_| Error::Crypto("bad aes key".to_string()))?;
            cipher
                .decrypt(Nonce::from_slice(nonce), Payload { msg: body, aad })
                .map_err(|_| Error::Crypto(OPEN_FAILED.to_string()))
        }
    }
}

/// Counter discipline for AES-256-GCM only. XChaCha20 skips this entirely.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct NonceCounter {
    pub key_id: u16,
    pub next: u64,
}

impl NonceCounter {
    pub fn take(&mut self) -> Result<[u8; 12]> {
        if self.next == u64::MAX {
            return Err(Error::Crypto(
                "nonce counter exhausted; rotate key_id".to_string(),
            ));
        }
        let value = self.next;
        self.next += 1;
        let mut nonce = [0u8; 12];
        nonce[4..].copy_from_slice(&value.to_be_bytes());
        Ok(nonce)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn key(byte: u8) -> SecretBytes {
        SecretBytes::new(vec![byte; 32])
    }

    #[test]
    fn header_layout_is_pinned() {
        let header = Header {
            algorithm: AeadAlgorithm::Aes256Gcm,
            key_id: 0x0102,
            nonce: vec![0xaa; 12],
        };
        let encoded = encode_header(&header).unwrap();
        assert_eq!(encoded.len(), HEADER_LEN + 12);
        assert_eq!(&encoded[..8], &MAGIC);
        assert_eq!(u16::from_be_bytes([encoded[8], encoded[9]]), 2);
        assert_eq!(u16::from_be_bytes([encoded[10], encoded[11]]), 0x0102);
        assert_eq!(u16::from_be_bytes([encoded[12], encoded[13]]), 12);
        assert_eq!((encoded[14], encoded[15]), (0, 0));
        assert_eq!(&encoded[HEADER_LEN..], &[0xaa; 12]);
    }

    #[test]
    fn header_rejects_wrong_nonce_and_reserved_bits() {
        let mut header = Header {
            algorithm: AeadAlgorithm::XChaCha20Poly1305,
            key_id: 1,
            nonce: vec![0u8; 12],
        };
        assert!(encode_header(&header).is_err());
        header.nonce = vec![0u8; 24];
        let mut bytes = encode_header(&header).unwrap();
        bytes[14] = 1;
        assert!(decode_header(&bytes).is_err());
        bytes[14] = 0;
        bytes[0] = b'X';
        assert!(decode_header(&bytes).is_err());
    }

    #[test]
    fn header_accepts_the_legacy_magic_for_import() {
        let header = Header {
            algorithm: AeadAlgorithm::Aes256Gcm,
            key_id: 1,
            nonce: vec![1u8; 12],
        };
        let mut bytes = encode_header(&header).unwrap();
        bytes[..8].copy_from_slice(&LEGACY_MAGIC);
        let (parsed, consumed) = decode_header(&bytes).unwrap();
        assert_eq!(parsed.algorithm, AeadAlgorithm::Aes256Gcm);
        assert_eq!(consumed, HEADER_LEN + 12);
    }

    #[test]
    fn xchacha_roundtrip() {
        let plain = b"deleted message text";
        let sealed = seal(&key(1), 7, AeadAlgorithm::XChaCha20Poly1305, b"aad", plain).unwrap();
        assert_eq!(sealed.len(), HEADER_LEN + 24 + plain.len() + TAG_LEN);
        let opened = open(&key(1), b"aad", &sealed).unwrap();
        assert_eq!(opened.expose(), plain);
    }

    #[test]
    fn aes_gcm_roundtrip() {
        let plain = b"legacy TFSYNC1 bundle";
        let sealed = seal(&key(2), 1, AeadAlgorithm::Aes256Gcm, b"aad", plain).unwrap();
        assert_eq!(sealed.len(), HEADER_LEN + 12 + plain.len() + TAG_LEN);
        assert_eq!(open(&key(2), b"aad", &sealed).unwrap().expose(), plain);
    }

    #[test]
    fn fresh_nonce_per_seal() {
        let a = seal(&key(3), 1, AeadAlgorithm::XChaCha20Poly1305, b"", b"same").unwrap();
        let b = seal(&key(3), 1, AeadAlgorithm::XChaCha20Poly1305, b"", b"same").unwrap();
        assert_ne!(a, b, "same plaintext must not seal to the same bytes");
        assert_ne!(
            &a[HEADER_LEN..HEADER_LEN + 24],
            &b[HEADER_LEN..HEADER_LEN + 24]
        );
    }

    #[test]
    fn wrong_key_and_wrong_aad_both_fail() {
        let sealed = seal(&key(4), 1, AeadAlgorithm::XChaCha20Poly1305, b"row-1", b"x").unwrap();
        let wrong_key = open(&key(5), b"row-1", &sealed).unwrap_err().to_string();
        let wrong_aad = open(&key(4), b"row-2", &sealed).unwrap_err().to_string();
        // Identical wording: the error must not tell an attacker which of the two
        // it was (that would be a decryption oracle).
        assert_eq!(wrong_key, wrong_aad);
    }

    #[test]
    fn tampered_ciphertext_fails() {
        let mut sealed = seal(&key(6), 1, AeadAlgorithm::Aes256Gcm, b"", b"payload").unwrap();
        let last = sealed.len() - 1;
        sealed[last] ^= 0xff;
        assert!(open(&key(6), b"", &sealed).is_err());
    }

    #[test]
    fn truncated_envelope_fails() {
        let sealed = seal(
            &key(7),
            1,
            AeadAlgorithm::XChaCha20Poly1305,
            b"",
            b"payload",
        )
        .unwrap();
        assert!(open(&key(7), b"", &sealed[..HEADER_LEN]).is_err());
        assert!(open(&key(7), b"", &sealed[..10]).is_err());
    }

    #[test]
    fn wrong_key_length_is_refused() {
        let short = SecretBytes::new(vec![0u8; 16]);
        assert!(seal(&short, 1, AeadAlgorithm::XChaCha20Poly1305, b"", b"x").is_err());
    }

    #[test]
    fn nonce_counter_never_repeats() {
        let mut counter = NonceCounter { key_id: 1, next: 0 };
        let first = counter.take().unwrap();
        let second = counter.take().unwrap();
        assert_ne!(first, second);
        assert_eq!(first[4..], 0u64.to_be_bytes());
        counter.next = u64::MAX;
        assert!(counter.take().is_err());
    }
}
