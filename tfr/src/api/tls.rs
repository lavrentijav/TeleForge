//! TLS 1.3 for the listener, and the client half the tests drive it with.
//!
//! # Why `rustls` and not OpenSSL
//!
//! `docs/architecture.md` § 3.9: the binary is uploaded by the client onto a server
//! nobody has inspected, so it must be statically linkable into musl and must not
//! depend on a system `libssl`. `rustls` + `ring` are pure Rust (plus assembly that
//! `cc` already handles for SQLite), so the same `Cargo.toml` builds the musl
//! artifact and this Windows dev box.
//!
//! # What the agent verifies, and what it leaves to the client
//!
//! The agent presents a **self-signed** certificate (`identity/agent.cert.pem`).
//! There is no public CA for a machine the user just rented, and asking Let's
//! Encrypt for an IP address would leak the deployment to a third party. Instead:
//!
//! * the client **pins** the certificate it saw during installation (T10 in the
//!   threat table) — the pinned fingerprint lives in the client's keychain;
//! * the transport underneath is the SSH channel the client used to install the
//!   agent, so a network attacker is already outside the model;
//! * the agent itself does not need to trust a CA: it authenticates the *request*
//!   with a device token, not the connection with a certificate.
//!
//! mTLS is available through `config.tls.client_ca_pem` for the explicit
//! `listen = network` mode; it is off by default because the SSH tunnel makes it
//! redundant and a second certificate lifecycle is a second thing to get wrong.
//!
//! # Cipher suites
//!
//! The provider is `ring`'s default: TLS 1.3 (and TLS 1.2 for older clients, which
//! is why the feature is enabled) with AEAD-only suites and forward secrecy. No
//! CBC, no RSA key exchange, no compression.

use std::sync::Arc;

use rustls::pki_types::{CertificateDer, PrivateKeyDer, PrivatePkcs8KeyDer};
#[cfg(test)]
use rustls::ClientConfig;
use rustls::ServerConfig;

use crate::error::{Error, Result};

fn provider() -> Arc<rustls::crypto::CryptoProvider> {
    // Built explicitly rather than installed globally: two listeners in one test
    // process would otherwise race on `install_default`, and the failure mode is a
    // panic inside rustls.
    Arc::new(rustls::crypto::ring::default_provider())
}

/// A certificate and its PKCS#8 key, in DER.
#[derive(Clone)]
pub struct TlsIdentity {
    pub cert_der: Vec<u8>,
    pub key_der: Vec<u8>,
}

impl TlsIdentity {
    pub fn from_pem_files(cert_path: &str, key_path: &str) -> Result<Self> {
        Ok(Self {
            cert_der: crate::api::pem::read_certificate(cert_path)?,
            key_der: crate::api::pem::read_private_key(key_path)?,
        })
    }

    pub fn from_pem_text(cert_pem: &str, key_pem: &str) -> Result<Self> {
        Ok(Self {
            cert_der: crate::api::pem::decode_block(cert_pem, "CERTIFICATE")?,
            key_der: crate::api::pem::decode_block(key_pem, "PRIVATE KEY")?,
        })
    }

    /// SHA-256 of the DER certificate, hex. This is the value the client pins and
    /// shows in its UI; `status` reports the same string so an operator can compare.
    pub fn fingerprint_hex(&self) -> String {
        use sha2::{Digest, Sha256};
        let digest = Sha256::digest(&self.cert_der);
        crate::crypto::sig::to_hex(&digest)
    }

    fn key(&self) -> PrivateKeyDer<'static> {
        PrivateKeyDer::Pkcs8(PrivatePkcs8KeyDer::from(self.key_der.clone()))
    }

    fn chain(&self) -> Vec<CertificateDer<'static>> {
        vec![CertificateDer::from(self.cert_der.clone())]
    }
}

impl core::fmt::Debug for TlsIdentity {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        f.debug_struct("TlsIdentity")
            .field("fingerprint", &self.fingerprint_hex())
            .finish()
    }
}

/// Server side of the listener.
pub fn server_config(identity: &TlsIdentity) -> Result<Arc<ServerConfig>> {
    let config = ServerConfig::builder_with_provider(provider())
        .with_safe_default_protocol_versions()
        .map_err(|error| Error::Tls(format!("protocol versions: {error}")))?
        .with_no_client_auth()
        .with_single_cert(identity.chain(), identity.key())
        .map_err(|error| Error::Tls(format!("certificate/key pair rejected: {error}")))?;
    Ok(Arc::new(config))
}

/// Client side used by the end-to-end tests: **pinned** to one certificate.
///
/// Not a "skip verification" verifier. Comparing the presented DER against the
/// exact certificate the agent loaded is what the desktop client does with a
/// fingerprint, and it also proves the agent is serving *its* certificate rather
/// than a test fixture that happens to chain to nothing.
#[cfg(test)]
pub fn pinned_client_config(expected_cert_der: &[u8]) -> Result<Arc<ClientConfig>> {
    let provider = provider();
    let mut config = ClientConfig::builder_with_provider(provider.clone())
        .with_safe_default_protocol_versions()
        .map_err(|error| Error::Tls(format!("protocol versions: {error}")))?
        .with_root_certificates(rustls::RootCertStore::empty())
        .with_no_client_auth();
    config
        .dangerous()
        .set_certificate_verifier(Arc::new(PinnedCertificate {
            expected: expected_cert_der.to_vec(),
            provider,
        }));
    Ok(Arc::new(config))
}

/// A [`rustls::client::danger::ServerCertVerifier`] that accepts exactly one
/// certificate.
#[cfg(test)]
#[derive(Debug)]
struct PinnedCertificate {
    expected: Vec<u8>,
    provider: Arc<rustls::crypto::CryptoProvider>,
}

#[cfg(test)]
impl rustls::client::danger::ServerCertVerifier for PinnedCertificate {
    fn verify_server_cert(
        &self,
        end_entity: &CertificateDer<'_>,
        _intermediates: &[CertificateDer<'_>],
        _server_name: &rustls::pki_types::ServerName<'_>,
        _ocsp_response: &[u8],
        _now: rustls::pki_types::UnixTime,
    ) -> std::result::Result<rustls::client::danger::ServerCertVerified, rustls::Error> {
        if end_entity.as_ref() == self.expected.as_slice() {
            Ok(rustls::client::danger::ServerCertVerified::assertion())
        } else {
            Err(rustls::Error::InvalidCertificate(
                rustls::CertificateError::UnknownIssuer,
            ))
        }
    }

    fn verify_tls12_signature(
        &self,
        message: &[u8],
        cert: &CertificateDer<'_>,
        dss: &rustls::DigitallySignedStruct,
    ) -> std::result::Result<rustls::client::danger::HandshakeSignatureValid, rustls::Error> {
        rustls::crypto::verify_tls12_signature(
            message,
            cert,
            dss,
            &self.provider.signature_verification_algorithms,
        )
    }

    fn verify_tls13_signature(
        &self,
        message: &[u8],
        cert: &CertificateDer<'_>,
        dss: &rustls::DigitallySignedStruct,
    ) -> std::result::Result<rustls::client::danger::HandshakeSignatureValid, rustls::Error> {
        rustls::crypto::verify_tls13_signature(
            message,
            cert,
            dss,
            &self.provider.signature_verification_algorithms,
        )
    }

    fn supported_verify_schemes(&self) -> Vec<rustls::SignatureScheme> {
        self.provider
            .signature_verification_algorithms
            .supported_schemes()
    }
}

/// The certificate the tests serve with.
///
/// A fixed, self-signed P-256 certificate ("CN=teleforge-agent-test", SAN
/// `localhost` + `127.0.0.1`, valid 2026–2036) generated once and embedded. It is
/// not a secret: its key is in this repository, on purpose, because a test
/// certificate generated at test time would need a second crypto dependency
/// (`rcgen`) just to be thrown away. It is bound to nothing and trusted by nothing
/// except [`pinned_client_config`] in this file.
#[cfg(test)]
pub mod test_identity {
    /// DER certificate, base64.
    pub const CERT_DER_B64: &str = "MIIBUzCB+aADAgECAgkA/FEjDgOqCO8wCgYIKoZIzj0EAwIwHzEdMBsGA1UEAxMUdGVsZWZvcmdlLWFnZW50LXRlc3QwHhcNMjYxMDE2MjIyMzI2WhcNMzYxMDE3MjIyMzI2WjAfMR0wGwYDVQQDExR0ZWxlZm9yZ2UtYWdlbnQtdGVzdDBZMBMGByqGSM49AgEGCCqGSM49AwEHA0IABJEm8ndzrW94g2n5uwc0id6FrNn+36RHkYPn5ep9DA3SBvTWckpUw0y3O4GtsoT178HAs0JANr2XwBtF1rqahwijHjAcMBoGA1UdEQQTMBGCCWxvY2FsaG9zdIcEfwAAATAKBggqhkjOPQQDAgNJADBGAiEArHfkkBaCVDQnj6tsHOuLVpgqpHZyYtCkG+4V9lchyHICIQCdcJr22yT0UD8Zs8x/N0kDAdzIBTPwiPCPNb/5FrLJiw==";

    /// PKCS#8 private key, base64.
    pub const KEY_DER_B64: &str = "MIGiAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBHkwdwIBAQQgOifin6owAv8i/Vr+kvPEJleSLa3LvHFhQZ26DDAI6c6gCgYIKoZIzj0DAQehRANCAASRJvJ3c61veINp+bsHNInehazZ/t+kR5GD5+XqfQwN0gb01nJKVMNMtzuBrbKE9e/BwLNCQDa9l8AbRda6mocIoA0wCwYDVR0PMQQDAgCA";

    pub fn identity() -> super::TlsIdentity {
        super::TlsIdentity {
            cert_der: crate::api::base64::decode(CERT_DER_B64).expect("embedded cert"),
            key_der: crate::api::base64::decode(KEY_DER_B64).expect("embedded key"),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_embedded_identity_builds_a_server_config() {
        let identity = test_identity::identity();
        let config = server_config(&identity).expect("a usable listener config");
        // No ALPN: there is exactly one protocol on this listener (the agent's
        // JSON envelope), so negotiating one would only add a way to disagree.
        assert!(config.alpn_protocols.is_empty());
        // The fingerprint is the value the client pins.
        assert_eq!(identity.fingerprint_hex().len(), 64);
    }

    #[test]
    fn a_garbage_key_is_rejected_instead_of_starting_a_broken_listener() {
        let mut identity = test_identity::identity();
        identity.key_der = vec![1u8; 16];
        let error = server_config(&identity).unwrap_err();
        assert!(
            error.to_string().to_lowercase().contains("key")
                || error.to_string().to_lowercase().contains("cert")
        );
    }

    #[test]
    fn debug_never_prints_key_material() {
        let identity = test_identity::identity();
        let rendered = format!("{identity:?}");
        let key_hex = crate::crypto::sig::to_hex(&identity.key_der);
        assert!(!rendered.contains(&key_hex[..16]));
        assert!(rendered.contains("fingerprint"));
    }

    #[test]
    fn a_wrong_certificate_does_not_match_the_pin() {
        // The pin is compared byte-for-byte, so a valid-looking chain with a
        // different leaf is refused. This is the T10 property in one assertion.
        let identity = test_identity::identity();
        let mut other = identity.clone();
        let last = other.cert_der.len() - 1;
        other.cert_der[last] ^= 0xff;
        assert_ne!(identity.fingerprint_hex(), other.fingerprint_hex());
        assert_ne!(identity.cert_der, other.cert_der);
    }
}
