//! Release manifest, signature verification, staged install and rollback.
//!
//! # Distribution
//!
//! ```text
//! GitHub Releases  (artifacts, immutable, per-tag)
//!   teleforge-agent-0.1.0-x86_64-unknown-linux-musl        (musl-static)
//!   teleforge-agent-0.1.0-aarch64-unknown-linux-musl
//!   teleforge-agent-0.1.0-x86_64-pc-windows-msvc.exe
//!   teleforge-agent-0.1.0-aarch64-apple-darwin
//! branch `agent-bin`  (manifest only, force-pushed, one commit — no history bloat)
//!   channels/stable/manifest.json
//!   channels/stable/manifest.json.sig
//!   channels/stable/root.pub.hex
//! ```
//!
//! Artifacts live in Releases and **not** in `agent-bin`: a 20 MiB binary per
//! platform per release would make the branch unclonable within a year. The branch
//! holds only the signed pointer, so "лежит в отдельной ветке" stays true for the
//! thing the client actually fetches first (the manifest), while the bytes come
//! from Releases with a `sha256` pinned in the signed manifest.
//!
//! # Trust chain
//!
//! The same Ed25519 root the project already uses for plugins
//! (`docs/PLUGIN_TRUST.md`, `plugin_trust.h`'s `kRootPublicKeyHex`) signs the
//! manifest. The client and the agent both have it compiled in
//! (`TF_AGENT_ROOT_PUBKEY_HEX`), so compromising the GitHub account alone is not
//! enough to ship a malicious agent: the attacker also needs the root private key,
//! which lives offline.
//!
//! Verification on the *client* (the side that installs):
//!
//! 1. fetch `manifest.json` + `manifest.json.sig`;
//! 2. `verify_manifest(...)` — signature over the canonical bytes, then the
//!    protocol/version gates;
//! 3. pick the artifact for the server's `uname -m` + libc;
//! 4. download, `sha256` must equal the manifest, then verify the *artifact*
//!    signature too (defence in depth: a manifest that is valid but whose
//!    `sha256` was swapped is caught by the artifact signature);
//! 5. upload, install, healthcheck, and only then commit the new slot.
//!
//! # Rollback
//!
//! ```text
//! <data_dir>/updater/
//!   state.json              last_generation, current_version, previous_version
//!   releases/<version>/teleforge-agent    0700, verified, not yet current
//!   current  -> releases/<version>
//!   previous -> releases/<version>
//! ```
//!
//! The switch is a `rename` of a symlink, so it is atomic. `previous` is kept
//! until the next successful healthcheck, which is what makes "the new version
//! crashes on start" a two-second recovery instead of a support ticket. The
//! *client* performs the switch over SSH (it owns the connection); this module
//! owns the decisions — which version, whether it may be installed, and whether a
//! rollback is required.

use std::cmp::Ordering;

use crate::crypto::sig;
use crate::error::{Error, Result};

/// Release channel. `beta` gets manifests with a higher `generation` but the same
/// trust chain; the client only offers `stable` by default.
pub const CHANNELS: &[&str] = &["stable", "beta"];

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub struct Version {
    pub major: u64,
    pub minor: u64,
    pub patch: u64,
}

impl Version {
    pub fn parse(text: &str) -> Result<Self> {
        let core = text.trim().trim_start_matches('v');
        let core = core.split(['-', '+']).next().unwrap_or("");
        let mut parts = core.split('.');
        let major = parts
            .next()
            .and_then(|p| p.parse::<u64>().ok())
            .ok_or_else(|| Error::Update(format!("bad version {text:?}")))?;
        let minor = parts
            .next()
            .and_then(|p| p.parse::<u64>().ok())
            .unwrap_or(0);
        let patch = parts
            .next()
            .and_then(|p| p.parse::<u64>().ok())
            .unwrap_or(0);
        Ok(Self {
            major,
            minor,
            patch,
        })
    }

    pub fn as_string(&self) -> String {
        format!("{}.{}.{}", self.major, self.minor, self.patch)
    }
}

pub fn compare_versions(left: &str, right: &str) -> Result<Ordering> {
    Ok(Version::parse(left)?.cmp(&Version::parse(right)?))
}

/// One downloadable artifact.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Artifact {
    /// Rust target triple, e.g. `x86_64-unknown-linux-musl`.
    pub triple: String,
    pub url: String,
    /// Lowercase hex SHA-256 of the exact bytes.
    pub sha256: String,
    pub size: u64,
    /// `musl` | `gnu` | `msvc` | `darwin`. The client prefers `musl` on Linux
    /// because it is the only variant that is guaranteed to run on an unknown
    /// distribution.
    pub libc: String,
    /// Set when TDLib is *not* linked (the "slim" build), so the client can prefer
    /// the full one when it has the bandwidth.
    pub slim: bool,
    /// Detached Ed25519 signature over the artifact bytes.
    pub sig: String,
}

/// Binary delta against a previous version. Optional: the client falls back to the
/// full artifact whenever a delta is absent, mismatched, or fails to verify.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Delta {
    pub from_version: String,
    pub to_version: String,
    pub triple: String,
    /// `zstd-patch` today; the field exists so the algorithm can change without a
    /// manifest schema change.
    pub algorithm: String,
    pub url: String,
    pub sha256: String,
    pub size: u64,
    /// SHA-256 of the *result*, so a delta cannot be used to produce a binary that
    /// differs from the signed full artifact.
    pub result_sha256: String,
}

/// The signed pointer published on `agent-bin`.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Manifest {
    pub schema: u32,
    pub channel: String,
    pub version: String,
    /// Monotone counter. The client refuses a manifest whose `generation` is not
    /// greater than the last one it applied, which is the anti-rollback control.
    pub generation: u64,
    /// Oldest desktop client allowed to talk to this agent.
    pub min_client_version: String,
    /// Oldest agent allowed to talk to the *current* client; the reverse gate.
    pub min_agent_version: String,
    /// TDLib the artifacts were built against, so a client can warn on a mismatch.
    pub tdlib_version: String,
    pub published_at: u64,
    pub artifacts: Vec<Artifact>,
    pub deltas: Vec<Delta>,
    /// `protocol` the artifacts speak, mirrored from
    /// [`crate::api::protocol::PROTOCOL_VERSION`].
    pub protocol: u16,
}

pub const MANIFEST_SCHEMA: u32 = 1;

impl Manifest {
    /// Canonical bytes that are signed.
    ///
    /// A dedicated canonical form, not "the JSON as served": JSON key order and
    /// whitespace are not stable across serialisers, and signing the served bytes
    /// would make the signature break on any reformatting — or, worse, invite
    /// "let's normalise before verifying", which is how signature checks get
    /// bypassed. The form is line-oriented and easy to reproduce in a shell
    /// (`openssl dgst` over `printf`), which matters when the project has to
    /// re-sign a manifest from a VPS.
    pub fn signing_bytes(&self) -> Vec<u8> {
        let mut out = String::new();
        out.push_str("TFAGENT-MANIFEST\n");
        out.push_str(&format!("schema {}\n", self.schema));
        out.push_str(&format!("channel {}\n", self.channel));
        out.push_str(&format!("version {}\n", self.version));
        out.push_str(&format!("generation {}\n", self.generation));
        out.push_str(&format!("min_client_version {}\n", self.min_client_version));
        out.push_str(&format!("min_agent_version {}\n", self.min_agent_version));
        out.push_str(&format!("tdlib_version {}\n", self.tdlib_version));
        out.push_str(&format!("protocol {}\n", self.protocol));
        out.push_str(&format!("published_at {}\n", self.published_at));
        // Sorted, so the signer and the verifier cannot disagree on ordering.
        let mut artifacts = self.artifacts.clone();
        artifacts.sort_by(|a, b| a.triple.cmp(&b.triple));
        for artifact in artifacts {
            out.push_str(&format!(
                "artifact {} {} {} {} {} {}\n",
                artifact.triple,
                artifact.libc,
                artifact.size,
                artifact.sha256,
                if artifact.slim { "slim" } else { "full" },
                artifact.sig
            ));
        }
        let mut deltas = self.deltas.clone();
        deltas.sort_by(|a, b| (&a.triple, &a.from_version).cmp(&(&b.triple, &b.from_version)));
        for delta in deltas {
            out.push_str(&format!(
                "delta {} {} {} {} {} {}\n",
                delta.triple,
                delta.from_version,
                delta.to_version,
                delta.algorithm,
                delta.size,
                delta.result_sha256
            ));
        }
        out.into_bytes()
    }

    pub fn to_json(&self) -> String {
        let artifacts: Vec<String> = self
            .artifacts
            .iter()
            .map(|a| {
                format!(
                    "{{\"triple\":\"{}\",\"libc\":\"{}\",\"url\":\"{}\",\"sha256\":\"{}\",\
                     \"size\":{},\"slim\":{},\"sig\":\"{}\"}}",
                    crate::api::protocol::json_escape(&a.triple),
                    a.libc,
                    crate::api::protocol::json_escape(&a.url),
                    a.sha256,
                    a.size,
                    a.slim,
                    a.sig
                )
            })
            .collect();
        let deltas: Vec<String> = self
            .deltas
            .iter()
            .map(|d| {
                format!(
                    "{{\"from\":\"{}\",\"to\":\"{}\",\"triple\":\"{}\",\"algorithm\":\"{}\",\
                     \"url\":\"{}\",\"sha256\":\"{}\",\"size\":{},\"result_sha256\":\"{}\"}}",
                    d.from_version,
                    d.to_version,
                    d.triple,
                    d.algorithm,
                    d.url,
                    d.sha256,
                    d.size,
                    d.result_sha256
                )
            })
            .collect();
        format!(
            "{{\"schema\":{},\"channel\":\"{}\",\"version\":\"{}\",\"generation\":{},\
             \"min_client_version\":\"{}\",\"min_agent_version\":\"{}\",\
             \"tdlib_version\":\"{}\",\"protocol\":{},\"published_at\":{},\
             \"artifacts\":[{}],\"deltas\":[{}]}}",
            self.schema,
            self.channel,
            self.version,
            self.generation,
            self.min_client_version,
            self.min_agent_version,
            self.tdlib_version,
            self.protocol,
            self.published_at,
            artifacts.join(","),
            deltas.join(",")
        )
    }

    /// Version-independent validation, run *before* touching the signature so a
    /// malformed manifest fails with a useful message.
    pub fn validate_shape(&self) -> Result<()> {
        if self.schema != MANIFEST_SCHEMA {
            return Err(Error::Update(format!(
                "manifest schema {} (this build speaks {MANIFEST_SCHEMA})",
                self.schema
            )));
        }
        if !CHANNELS.contains(&self.channel.as_str()) {
            return Err(Error::Update(format!("unknown channel {:?}", self.channel)));
        }
        Version::parse(&self.version)?;
        Version::parse(&self.min_client_version)?;
        Version::parse(&self.min_agent_version)?;
        if self.artifacts.is_empty() {
            return Err(Error::Update("manifest lists no artifacts".to_string()));
        }
        for artifact in &self.artifacts {
            if artifact.sha256.len() != 64 || sig::parse_hex(&artifact.sha256).is_err() {
                return Err(Error::Update(format!(
                    "artifact {} has a malformed sha256",
                    artifact.triple
                )));
            }
            if artifact.size == 0 {
                return Err(Error::Update(format!(
                    "artifact {} has size 0",
                    artifact.triple
                )));
            }
            match sig::parse_hex(&artifact.sig) {
                Ok(bytes) if bytes.len() == sig::SIG_LEN => {}
                _ => {
                    return Err(Error::Update(format!(
                        "artifact {} has a malformed signature",
                        artifact.triple
                    )))
                }
            }
        }
        Ok(())
    }

    /// Full verification: shape, then Ed25519 over [`Self::signing_bytes`].
    pub fn verify(
        &self,
        root_pubkey: &[u8; sig::PUBKEY_LEN],
        signature: &[u8; sig::SIG_LEN],
    ) -> Result<()> {
        self.validate_shape()?;
        sig::verify(root_pubkey, &self.signing_bytes(), signature)
    }

    /// Refuse to install a manifest the running client is too old for.
    pub fn assert_client_supported(&self, client_version: &str) -> Result<()> {
        if compare_versions(client_version, &self.min_client_version)? == Ordering::Less {
            return Err(Error::Update(format!(
                "client {client_version} is older than min_client_version {}",
                self.min_client_version
            )));
        }
        Ok(())
    }

    /// Anti-rollback. Equal is rejected too: re-installing the version that is
    /// already running is a no-op the client should not spend a restart on, and
    /// accepting `==` would let a replayed old manifest through if the generation
    /// were ever reused by mistake.
    pub fn assert_not_rollback(&self, last_generation: u64) -> Result<()> {
        if self.generation <= last_generation {
            return Err(Error::Update(format!(
                "manifest generation {} is not newer than the applied {last_generation}",
                self.generation
            )));
        }
        Ok(())
    }

    /// Pick the best artifact for a detected target.
    ///
    /// Order of preference: exact triple, then any triple with the same
    /// architecture+OS, then `musl` over `gnu` on Linux, then full over slim.
    pub fn select_artifact(&self, triple: &str, libc: &str) -> Result<&Artifact> {
        if let Some(exact) = self.artifacts.iter().find(|a| a.triple == triple) {
            return Ok(exact);
        }
        let arch = triple.split('-').next().unwrap_or("");
        let candidates: Vec<&Artifact> = self
            .artifacts
            .iter()
            .filter(|a| a.triple.starts_with(arch))
            .collect();
        if let Some(musl) = candidates
            .iter()
            .find(|a| a.libc == "musl" && a.triple.contains("linux"))
        {
            return Ok(musl);
        }
        if let Some(same_libc) = candidates.iter().find(|a| a.libc == libc) {
            return Ok(same_libc);
        }
        if let Some(any) = candidates.first() {
            return Ok(any);
        }
        Err(Error::Update(format!(
            "no artifact for {triple}/{libc}; manifest has {:?}",
            self.artifacts
                .iter()
                .map(|a| a.triple.as_str())
                .collect::<Vec<_>>()
        )))
    }

    /// A delta is usable only if it starts from exactly the running version.
    pub fn select_delta(&self, triple: &str, from_version: &str) -> Option<&Delta> {
        self.deltas
            .iter()
            .find(|d| d.triple == triple && d.from_version == from_version)
    }
}

/// Whether an update should be installed, and why not if it should not.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Decision {
    Install {
        version: String,
        generation: u64,
        triple: String,
        via_delta: bool,
    },
    UpToDate {
        version: String,
    },
    Refused {
        reason: String,
    },
}

/// The whole policy in one pure function, so it is testable without a network.
pub fn decide(
    manifest: &Manifest,
    running_version: &str,
    client_version: &str,
    last_generation: u64,
    triple: &str,
    libc: &str,
) -> Decision {
    if let Err(error) = manifest.validate_shape() {
        return Decision::Refused {
            reason: error.to_string(),
        };
    }
    if let Err(error) = manifest.assert_client_supported(client_version) {
        return Decision::Refused {
            reason: error.to_string(),
        };
    }
    if let Err(error) = manifest.assert_not_rollback(last_generation) {
        return Decision::Refused {
            reason: error.to_string(),
        };
    }
    let newer = match compare_versions(&manifest.version, running_version) {
        Ok(Ordering::Greater) => true,
        Ok(_) => false,
        Err(error) => {
            return Decision::Refused {
                reason: error.to_string(),
            }
        }
    };
    if !newer {
        return Decision::UpToDate {
            version: running_version.to_string(),
        };
    }
    let artifact = match manifest.select_artifact(triple, libc) {
        Ok(artifact) => artifact,
        Err(error) => {
            return Decision::Refused {
                reason: error.to_string(),
            }
        }
    };
    Decision::Install {
        version: manifest.version.clone(),
        generation: manifest.generation,
        triple: artifact.triple.clone(),
        via_delta: manifest
            .select_delta(&artifact.triple, running_version)
            .is_some(),
    }
}

/// Layout of the update directory. Shared by the agent (which stages) and the
/// client (which performs the switch over SSH), so both must agree — hence one
/// definition.
#[derive(Clone, Debug)]
pub struct InstallPlan {
    pub root: String,
    pub releases_dir: String,
    pub state_file: String,
    pub current_link: String,
    pub previous_link: String,
}

impl InstallPlan {
    pub fn new(data_dir: &str) -> Self {
        Self {
            root: format!("{data_dir}/updater"),
            releases_dir: format!("{data_dir}/updater/releases"),
            state_file: format!("{data_dir}/updater/state.json"),
            current_link: format!("{data_dir}/updater/current"),
            previous_link: format!("{data_dir}/updater/previous"),
        }
    }

    pub fn release_dir(&self, version: &str) -> String {
        format!("{}/{}", self.releases_dir, version)
    }

    pub fn release_binary(&self, version: &str, binary_name: &str) -> String {
        format!("{}/{}/{}", self.releases_dir, version, binary_name)
    }

    /// Steps the client performs, in order. Returned as data so the client's UI
    /// can show them and so a failure names the exact step.
    pub fn steps(&self, version: &str, binary_name: &str) -> Vec<String> {
        vec![
            format!("mkdir -p -m 0700 {}", self.releases_dir),
            format!("mkdir -p -m 0700 {}", self.release_dir(version)),
            format!(
                "upload <temp> {} && chmod 0700 {}",
                self.release_binary(version, binary_name),
                self.release_binary(version, binary_name)
            ),
            format!(
                "{bin} --self-test --data-dir <data_dir>",
                bin = self.release_binary(version, binary_name)
            ),
            // Atomic: rename over an existing symlink replaces it in one step.
            format!(
                "ln -sfn {} {}",
                self.release_dir(version),
                self.current_link
            ),
            "systemctl --user restart teleforge-agent".to_string(),
            "wait for `status` to report healthy".to_string(),
        ]
    }

    /// Rollback steps: swap `previous` back in and restart.
    pub fn rollback_steps(&self) -> Vec<String> {
        vec![
            format!(
                "test -L {prev} && ln -sfn $(readlink {prev}) {cur}",
                prev = self.previous_link,
                cur = self.current_link
            ),
            "systemctl --user restart teleforge-agent".to_string(),
            "verify `status` before clearing the failure".to_string(),
        ]
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum HealthcheckVerdict {
    /// The new version answered `status` and reported every component healthy.
    Healthy,
    /// It did not. The caller must roll back.
    Unhealthy { reason: String },
}

/// Decide after a restart. `probe` is the parsed `status` output of the *new*
/// process; passing it in keeps this pure and testable.
pub fn healthcheck(
    probe: Result<&str>,
    expected_version: &str,
    expected_protocol: u16,
) -> HealthcheckVerdict {
    let body = match probe {
        Ok(body) => body,
        Err(error) => {
            return HealthcheckVerdict::Unhealthy {
                reason: error.to_string(),
            }
        }
    };
    match crate::api::protocol::extract_str(body, "agent_version") {
        Some(version) if version == expected_version => {}
        Some(version) => {
            return HealthcheckVerdict::Unhealthy {
                reason: format!("agent reports version {version}, expected {expected_version}"),
            }
        }
        None => {
            return HealthcheckVerdict::Unhealthy {
                reason: "status has no agent_version".to_string(),
            }
        }
    }
    match crate::api::protocol::extract_number(body, "protocol") {
        Some(protocol) if protocol == u64::from(expected_protocol) => {}
        Some(protocol) => {
            return HealthcheckVerdict::Unhealthy {
                reason: format!("agent speaks protocol {protocol}, expected {expected_protocol}"),
            }
        }
        None => {
            return HealthcheckVerdict::Unhealthy {
                reason: "status has no protocol".to_string(),
            }
        }
    }
    if body.contains("\"failed(") {
        return HealthcheckVerdict::Unhealthy {
            reason: "status reports a failed component".to_string(),
        };
    }
    HealthcheckVerdict::Healthy
}

#[cfg(test)]
mod tests {
    use super::*;

    fn artifact(triple: &str, libc: &str, slim: bool) -> Artifact {
        Artifact {
            triple: triple.to_string(),
            url: format!("https://example.invalid/{triple}"),
            sha256: "a".repeat(64),
            size: 1_000,
            libc: libc.to_string(),
            slim,
            sig: "b".repeat(128),
        }
    }

    fn manifest() -> Manifest {
        Manifest {
            schema: MANIFEST_SCHEMA,
            channel: "stable".to_string(),
            version: "0.2.0".to_string(),
            generation: 7,
            min_client_version: "0.1.0".to_string(),
            min_agent_version: "0.1.0".to_string(),
            tdlib_version: "1.8.29".to_string(),
            published_at: 1_700_000_000,
            protocol: 1,
            artifacts: vec![
                artifact("x86_64-unknown-linux-musl", "musl", false),
                artifact("x86_64-unknown-linux-gnu", "gnu", false),
                artifact("aarch64-unknown-linux-musl", "musl", false),
            ],
            deltas: vec![Delta {
                from_version: "0.1.0".to_string(),
                to_version: "0.2.0".to_string(),
                triple: "x86_64-unknown-linux-musl".to_string(),
                algorithm: "zstd-patch".to_string(),
                url: "https://example.invalid/delta".to_string(),
                sha256: "c".repeat(64),
                size: 200,
                result_sha256: "a".repeat(64),
            }],
        }
    }

    #[test]
    fn signing_bytes_are_order_independent() {
        let mut shuffled = manifest();
        shuffled.artifacts.reverse();
        assert_eq!(shuffled.signing_bytes(), manifest().signing_bytes());
        assert!(shuffled.signing_bytes().starts_with(b"TFAGENT-MANIFEST\n"));
    }

    #[test]
    fn signing_bytes_change_with_every_signed_field() {
        let base = manifest().signing_bytes();
        let mut other = manifest();
        other.version = "0.3.0".to_string();
        assert_ne!(base, other.signing_bytes());

        let mut gen = manifest();
        gen.generation = 8;
        assert_ne!(base, gen.signing_bytes());

        let mut sha = manifest();
        sha.artifacts[0].sha256 = "d".repeat(64);
        assert_ne!(base, sha.signing_bytes());
    }

    #[test]
    fn shape_validation_catches_bad_hashes() {
        let mut bad = manifest();
        bad.artifacts[0].sha256 = "short".to_string();
        assert!(bad.validate_shape().is_err());

        let mut no_artifacts = manifest();
        no_artifacts.artifacts.clear();
        assert!(no_artifacts.validate_shape().is_err());

        let mut bad_sig = manifest();
        bad_sig.artifacts[0].sig = "00".to_string();
        assert!(bad_sig.validate_shape().is_err());

        let mut bad_channel = manifest();
        bad_channel.channel = "nightly".to_string();
        assert!(bad_channel.validate_shape().is_err());
    }

    #[test]
    fn artifact_selection_prefers_musl_on_linux() {
        let manifest = manifest();
        let picked = manifest
            .select_artifact("x86_64-unknown-linux-gnu", "gnu")
            .unwrap();
        // An exact triple wins over the musl preference.
        assert_eq!(picked.triple, "x86_64-unknown-linux-gnu");

        let picked = manifest
            .select_artifact("x86_64-unknown-linux-musl", "musl")
            .unwrap();
        assert_eq!(picked.triple, "x86_64-unknown-linux-musl");
    }

    #[test]
    fn artifact_selection_fails_closed() {
        let manifest = manifest();
        let error = manifest
            .select_artifact("riscv64gc-unknown-linux-musl", "musl")
            .unwrap_err();
        assert!(matches!(error, Error::Update(_)));
    }

    #[test]
    fn decide_installs_newer_and_refuses_rollback() {
        let manifest = manifest();
        match decide(
            &manifest,
            "0.1.0",
            "0.1.0",
            6,
            "x86_64-unknown-linux-musl",
            "musl",
        ) {
            Decision::Install {
                version, via_delta, ..
            } => {
                assert_eq!(version, "0.2.0");
                assert!(via_delta);
            }
            other => panic!("expected install, got {other:?}"),
        }
        match decide(
            &manifest,
            "0.1.0",
            "0.1.0",
            7,
            "x86_64-unknown-linux-musl",
            "musl",
        ) {
            Decision::Refused { reason } => assert!(reason.contains("generation")),
            other => panic!("expected refusal, got {other:?}"),
        }
    }

    #[test]
    fn decide_reports_up_to_date() {
        let mut manifest = manifest();
        manifest.generation = 99;
        match decide(
            &manifest,
            "0.2.0",
            "0.1.0",
            1,
            "x86_64-unknown-linux-musl",
            "musl",
        ) {
            Decision::UpToDate { version } => assert_eq!(version, "0.2.0"),
            other => panic!("expected up to date, got {other:?}"),
        }
    }

    #[test]
    fn decide_refuses_when_the_client_is_too_old() {
        let mut manifest = manifest();
        manifest.min_client_version = "9.0.0".to_string();
        match decide(
            &manifest,
            "0.1.0",
            "0.1.0",
            1,
            "x86_64-unknown-linux-musl",
            "musl",
        ) {
            Decision::Refused { reason } => assert!(reason.contains("older")),
            other => panic!("expected refusal, got {other:?}"),
        }
    }

    #[test]
    fn version_compare_is_numeric() {
        assert_eq!(
            compare_versions("0.10.0", "0.9.9").unwrap(),
            Ordering::Greater
        );
        assert_eq!(compare_versions("1.0.0", "1.0.0").unwrap(), Ordering::Equal);
        assert_eq!(
            compare_versions("v0.2.0", "0.1.9").unwrap(),
            Ordering::Greater
        );
        assert!(compare_versions("nope", "1.0.0").is_err());
    }

    #[test]
    fn healthcheck_accepts_a_good_status() {
        let body = r#"{"agent_version":"0.2.0","protocol":1,"components":{}}"#;
        assert_eq!(
            healthcheck(Ok(body), "0.2.0", 1),
            HealthcheckVerdict::Healthy
        );
    }

    #[test]
    fn healthcheck_rejects_version_and_component_mismatch() {
        let wrong_version = r#"{"agent_version":"0.1.0","protocol":1}"#;
        assert!(matches!(
            healthcheck(Ok(wrong_version), "0.2.0", 1),
            HealthcheckVerdict::Unhealthy { .. }
        ));
        let failed = r#"{"agent_version":"0.2.0","protocol":1,"tdlib":"failed(attempts=3)"}"#;
        assert!(matches!(
            healthcheck(Ok(failed), "0.2.0", 1),
            HealthcheckVerdict::Unhealthy { .. }
        ));
        assert!(matches!(
            healthcheck(Err(Error::Io(std::io::Error::other("refused"))), "0.2.0", 1),
            HealthcheckVerdict::Unhealthy { .. }
        ));
    }

    #[test]
    fn install_and_rollback_steps_are_atomic() {
        let plan = InstallPlan::new("/var/lib/teleforge-agent");
        let steps = plan.steps("0.2.0", "teleforge-agent");
        assert!(steps.iter().any(|s| s.contains("ln -sfn")));
        assert!(steps.iter().any(|s| s.contains("--self-test")));
        assert!(!steps.iter().any(|s| s.contains("rm -rf")));
        let rollback = plan.rollback_steps();
        assert!(rollback.iter().any(|s| s.contains("readlink")));
        assert_eq!(
            plan.release_binary("0.2.0", "teleforge-agent"),
            "/var/lib/teleforge-agent/updater/releases/0.2.0/teleforge-agent"
        );
    }
}
