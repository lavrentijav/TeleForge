//! Signatures and hashes.
//!
//! Ed25519 is already the project's trust primitive: `docs/PLUGIN_TRUST.md`
//! builds a root → developer → plugin chain out of it, and
//! `Telegram/SourceFiles/ayu/features/plugins/plugin_trust.cpp` verifies it. The
//! agent's release manifest and the desktop client's device certificates use the
//! same curve for the same reason: 32-byte keys, 64-byte signatures, no
//! parameter choices to get wrong, and one verifier implementation already in
//! the tree.

use crate::error::{Error, Result};
use crate::secret::SecretBytes;

pub const PUBKEY_LEN: usize = 32;
pub const SIG_LEN: usize = 64;

/// Verify `signature` over `message` with a 32-byte raw Ed25519 public key.
pub fn verify(pubkey: &[u8; PUBKEY_LEN], message: &[u8], signature: &[u8; SIG_LEN]) -> Result<()> {
    let _ = (pubkey, message, signature);
    // TODO(ed25519-dalek): VerifyingKey::from_bytes(pubkey) then
    // verify_strict(message, &Signature::from_bytes(signature)). `verify_strict`
    // (not `verify`) rejects small-order public keys, which is what stops a
    // "signature that verifies under a key the attacker chose".
    Err(Error::NotImplemented(
        "ed25519_dalek::VerifyingKey::verify_strict",
    ))
}

/// SHA-256 as lowercase hex.
pub fn sha256_hex(bytes: &[u8]) -> Result<String> {
    let _ = bytes;
    // TODO(sha2): Sha256::digest + hex encode.
    Err(Error::NotImplemented("sha2::Sha256"))
}

/// Placeholder used by [`crate::crypto::kdf`]'s re-export so the module graph is
/// complete before `sha2` lands.
pub fn sha256_hex_or_stub(bytes: &[u8]) -> Result<String> {
    sha256_hex(bytes)
}

/// Decode exactly 32 bytes from 64 hex characters.
pub fn parse_hex_32(input: &str) -> Result<[u8; PUBKEY_LEN]> {
    let trimmed = input.trim();
    if trimmed.len() != PUBKEY_LEN * 2 {
        return Err(Error::Invalid(format!(
            "expected {} hex chars, got {}",
            PUBKEY_LEN * 2,
            trimmed.len()
        )));
    }
    let mut out = [0u8; PUBKEY_LEN];
    let raw = trimmed.as_bytes();
    for i in 0..PUBKEY_LEN {
        let hi = hex_nibble(raw[i * 2])?;
        let lo = hex_nibble(raw[i * 2 + 1])?;
        out[i] = (hi << 4) | lo;
    }
    Ok(out)
}

/// Decode any-length hex. Used for artifact digests and device ids.
pub fn parse_hex(input: &str) -> Result<Vec<u8>> {
    let trimmed = input.trim();
    if trimmed.len() % 2 != 0 {
        return Err(Error::Invalid("odd-length hex string".to_string()));
    }
    let raw = trimmed.as_bytes();
    let mut out = Vec::with_capacity(raw.len() / 2);
    for pair in raw.chunks(2) {
        out.push((hex_nibble(pair[0])? << 4) | hex_nibble(pair[1])?);
    }
    Ok(out)
}

pub fn to_hex(bytes: &[u8]) -> String {
    const HEX: &[u8; 16] = b"0123456789abcdef";
    let mut out = String::with_capacity(bytes.len() * 2);
    for byte in bytes {
        out.push(HEX[(byte >> 4) as usize] as char);
        out.push(HEX[(byte & 0x0F) as usize] as char);
    }
    out
}

/// Sign with a private key. Only the build/CI tooling signs; the agent process
/// never holds a signing key, so this is `#[cfg(test)]`-adjacent by design and
/// the shipped binary cannot be tricked into signing anything.
pub fn sign(private_key: &SecretBytes, message: &[u8]) -> Result<[u8; SIG_LEN]> {
    if private_key.len() != 32 && private_key.len() != 64 {
        return Err(Error::Crypto(
            "signing key must be 32 (seed) or 64 (expanded) bytes".to_string(),
        ));
    }
    let _ = message;
    Err(Error::NotImplemented("ed25519_dalek::SigningKey::sign"))
}

fn hex_nibble(byte: u8) -> Result<u8> {
    match byte {
        b'0'..=b'9' => Ok(byte - b'0'),
        b'a'..=b'f' => Ok(byte - b'a' + 10),
        b'A'..=b'F' => Ok(byte - b'A' + 10),
        other => Err(Error::Invalid(format!("bad hex digit {:?}", other as char))),
    }
}

/// The release root public key, compiled into every client build and every agent
/// build. `None` here means the build script has not injected it yet; a release
/// build must fail rather than fall back to "trust anything".
pub const COMPILED_ROOT_PUBKEY_HEX: Option<&str> = option_env!("TF_AGENT_ROOT_PUBKEY_HEX");

pub fn compiled_root_pubkey() -> Result<[u8; PUBKEY_LEN]> {
    match COMPILED_ROOT_PUBKEY_HEX {
        Some(hex) => parse_hex_32(hex),
        None => Err(Error::Update(
            "this binary was built without TF_AGENT_ROOT_PUBKEY_HEX; \
             it cannot verify release manifests and must not be shipped"
                .to_string(),
        )),
    }
}
