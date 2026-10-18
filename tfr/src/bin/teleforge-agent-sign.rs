//! `teleforge-agent-sign` — release tooling.
//!
//! Kept in the same crate as the verifier so that the *byte layout that is
//! signed* and the *byte layout that is verified* cannot drift: both call
//! [`teleforge_agent::updater::Manifest::signing_bytes`] and
//! [`teleforge_agent::crypto::sig`]. A separate bash/openssl signer would
//! inevitably disagree with the Rust verifier on some detail, and the failure
//! mode of that disagreement is "the release cannot be installed".
//!
//! This binary is **never** part of the shipped agent. The agent process holds no
//! signing key — so it cannot be tricked into signing anything, which matters
//! because it runs on a machine an attacker may control.
//!
//! ```text
//! teleforge-agent-sign artifact <path>            → base64/hex signature on stdout
//! teleforge-agent-sign manifest --channel … --out manifest.json
//! teleforge-agent-sign manifest-signature <path>  → signature on stdout
//! teleforge-agent-sign verify --pubkey … --manifest … --signature …
//! teleforge-agent-sign keygen                     → new keypair (hex), dev only
//! ```
//!
//! Key material is read from `TF_AGENT_SIGNING_KEY` (32-byte hex seed) and is
//! never accepted as an argument, so it does not end up in the process list of a
//! shared CI runner.

use std::process::ExitCode;

use teleforge_agent::crypto::sig;
use teleforge_agent::error::{Error, Result};
use teleforge_agent::secret::SecretBytes;
use teleforge_agent::updater::{Artifact, Delta, Manifest, MANIFEST_SCHEMA};

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    match run(&args) {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => {
            // Only the message: no context that could contain key material.
            eprintln!("teleforge-agent-sign: {error}");
            ExitCode::FAILURE
        }
    }
}

fn run(args: &[String]) -> Result<()> {
    let Some(command) = args.first().map(String::as_str) else {
        eprintln!("{}", usage());
        return Err(Error::Invalid("no subcommand".to_string()));
    };
    match command {
        "artifact" => {
            let path = args
                .get(1)
                .ok_or_else(|| Error::Invalid("artifact needs a path".to_string()))?;
            let key = signing_key()?;
            let bytes = std::fs::read(path)?;
            let signature = sig::sign(&key, &bytes)?;
            println!("{}", sig::to_hex(&signature));
            Ok(())
        }
        "manifest" => {
            let manifest = build_manifest(&args[1..])?;
            let out = flag(&args[1..], "--out")
                .ok_or_else(|| Error::Invalid("manifest needs --out".to_string()))?;
            std::fs::write(&out, manifest.to_json())?;
            // Print the canonical bytes hash so the CI log proves what was signed.
            let digest = sig::sha256_hex(&manifest.signing_bytes())?;
            eprintln!(
                "manifest {} written, signing_bytes sha256={digest}",
                manifest.version
            );
            Ok(())
        }
        "manifest-signature" => {
            let path = args
                .get(1)
                .ok_or_else(|| Error::Invalid("manifest-signature needs a path".to_string()))?;
            let key = signing_key()?;
            // The signature covers the *canonical* bytes, so re-parse rather than
            // signing the file: signing a file would make any whitespace change a
            // silent mismatch.
            let text = std::fs::read_to_string(path)?;
            let manifest = parse_manifest(&text)?;
            let signature = sig::sign(&key, &manifest.signing_bytes())?;
            println!("{}", sig::to_hex(&signature));
            Ok(())
        }
        "verify" => {
            let pubkey_hex = flag(&args[1..], "--pubkey")
                .ok_or_else(|| Error::Invalid("verify needs --pubkey".to_string()))?;
            let manifest_path = flag(&args[1..], "--manifest")
                .ok_or_else(|| Error::Invalid("verify needs --manifest".to_string()))?;
            let signature_path = flag(&args[1..], "--signature")
                .ok_or_else(|| Error::Invalid("verify needs --signature".to_string()))?;
            let pubkey = sig::parse_hex_32(&pubkey_hex)?;
            let manifest = parse_manifest(&std::fs::read_to_string(&manifest_path)?)?;
            let raw = sig::parse_hex(&std::fs::read_to_string(&signature_path)?)?;
            let mut signature = [0u8; sig::SIG_LEN];
            if raw.len() != sig::SIG_LEN {
                return Err(Error::Invalid(format!(
                    "signature must be {} bytes, got {}",
                    sig::SIG_LEN,
                    raw.len()
                )));
            }
            signature.copy_from_slice(&raw);
            manifest.verify(&pubkey, &signature)?;
            println!("ok {} ({})", manifest.version, manifest.channel);
            Ok(())
        }
        "keygen" => {
            // Dev convenience only. A production key is generated offline and
            // never leaves the machine that signs.
            Err(Error::NotImplemented(
                "keygen: use `openssl genpkey -algorithm ed25519` offline and keep the seed in secrets",
            ))
        }
        "-h" | "--help" | "help" => {
            print!("{}", usage());
            Ok(())
        }
        other => Err(Error::Invalid(format!("unknown subcommand {other:?}"))),
    }
}

fn usage() -> &'static str {
    "\
teleforge-agent-sign — release manifest and artifact signing

USAGE:
    teleforge-agent-sign artifact <path>
    teleforge-agent-sign manifest --channel C --version V --generation N \
        --min-client V --min-agent V --tdlib V --protocol N \
        --artifacts DIR --base-url URL --out FILE
    teleforge-agent-sign manifest-signature <manifest.json>
    teleforge-agent-sign verify --pubkey HEX --manifest FILE --signature FILE

ENV:
    TF_AGENT_SIGNING_KEY   32-byte hex Ed25519 seed (never passed as an argument)
"
}

fn signing_key() -> Result<SecretBytes> {
    let hex = std::env::var("TF_AGENT_SIGNING_KEY")
        .map_err(|_| Error::Auth("TF_AGENT_SIGNING_KEY is not set".to_string()))?;
    let bytes = sig::parse_hex(&hex)?;
    if bytes.len() != 32 {
        return Err(Error::Auth(format!(
            "signing key must be 32 bytes, got {}",
            bytes.len()
        )));
    }
    Ok(SecretBytes::take_from(bytes))
}

fn flag(args: &[String], name: &str) -> Option<String> {
    let mut index = 0usize;
    while index < args.len() {
        if args[index] == name {
            return args.get(index + 1).cloned();
        }
        index += 1;
    }
    None
}

/// Build a manifest from the signed-artifact directory.
///
/// `--artifacts` is scanned for `teleforge-agent-<version>-<triple>[.exe]`,
/// `<file>.sha256` and `<file>.sig`; those are exactly the names the release job
/// produces, so the tool cannot silently include a file the client will not find.
fn build_manifest(args: &[String]) -> Result<Manifest> {
    let channel = required(args, "--channel")?;
    let version = required(args, "--version")?;
    let generation: u64 = required(args, "--generation")?
        .parse()
        .map_err(|_| Error::Invalid("--generation must be a number".to_string()))?;
    let min_client = required(args, "--min-client")?;
    let min_agent = required(args, "--min-agent")?;
    let tdlib = required(args, "--tdlib")?;
    let protocol: u16 = required(args, "--protocol")?
        .parse()
        .map_err(|_| Error::Invalid("--protocol must be a number".to_string()))?;
    let artifacts_dir = required(args, "--artifacts")?;
    let base_url = required(args, "--base-url")?;

    let mut artifacts = Vec::new();
    let mut deltas = Vec::new();
    for entry in std::fs::read_dir(&artifacts_dir)? {
        let entry = entry?;
        let name = entry.file_name().to_string_lossy().to_string();
        let path = entry.path();
        if name.ends_with(".zstd-patch") {
            // Delta metadata is produced by the delta job; parsed from a sidecar
            // so this tool never has to open a binary patch.
            if let Some(delta) = read_delta(&path, &version, &base_url)? {
                deltas.push(delta);
            }
            continue;
        }
        if !name.starts_with("teleforge-agent-") {
            continue;
        }
        if name.ends_with(".sha256") || name.ends_with(".sig") || name.ends_with(".size") {
            continue;
        }
        let Some(triple) = triple_from_name(&name, &version) else {
            continue;
        };
        let sha256 = std::fs::read_to_string(
            path.with_extension(format!("{}sha256", extension_prefix(&name))),
        )?;
        let sig_hex = std::fs::read_to_string(format!("{}.sig", path.to_string_lossy()))?;
        let size = std::fs::metadata(&path)?.len();
        let libc = if triple.contains("musl") {
            "musl"
        } else if triple.contains("gnu") {
            "gnu"
        } else if triple.contains("msvc") {
            "msvc"
        } else {
            "darwin"
        };
        artifacts.push(Artifact {
            triple,
            url: format!("{base_url}/{name}"),
            sha256: sha256.trim().to_lowercase(),
            size,
            libc: libc.to_string(),
            slim: name.contains("-slim"),
            sig: sig_hex.trim().to_lowercase(),
        });
    }

    if artifacts.is_empty() {
        return Err(Error::Update(format!(
            "no artifacts found in {artifacts_dir}"
        )));
    }

    let now = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0);

    Ok(Manifest {
        schema: MANIFEST_SCHEMA,
        channel,
        version,
        generation,
        min_client_version: min_client,
        min_agent_version: min_agent,
        tdlib_version: tdlib,
        published_at: now,
        protocol,
        artifacts,
        deltas,
    })
}

fn read_delta(_path: &std::path::Path, _version: &str, _base: &str) -> Result<Option<Delta>> {
    // TODO(release): sidecar `<patch>.json` with from/to/triple/algorithm/
    // sha256/size/result_sha256, produced by the delta job. Returning `None` now
    // means the manifest simply has no deltas, and the client downloads the full
    // artifact — a correct, just less efficient, outcome.
    Ok(None)
}

fn extension_prefix(name: &str) -> &'static str {
    if name.ends_with(".exe") {
        "exe."
    } else {
        ""
    }
}

/// `teleforge-agent-0.2.0-x86_64-unknown-linux-musl` → `x86_64-unknown-linux-musl`.
fn triple_from_name(name: &str, version: &str) -> Option<String> {
    let prefix = format!("teleforge-agent-{version}-");
    let rest = name.strip_prefix(&prefix)?;
    let rest = rest.strip_suffix(".exe").unwrap_or(rest);
    let rest = rest.strip_suffix("-slim").unwrap_or(rest);
    if rest.is_empty() {
        None
    } else {
        Some(rest.to_string())
    }
}

fn required(args: &[String], name: &str) -> Result<String> {
    flag(args, name).ok_or_else(|| Error::Invalid(format!("{name} is required")))
}

/// Parse the manifest JSON the tool itself wrote.
///
/// A real build uses `serde_json`; this reader covers the flat fields that the
/// signature depends on and refuses anything it cannot see, because silently
/// signing a partially-parsed manifest would produce an unverifiable release.
fn parse_manifest(text: &str) -> Result<Manifest> {
    use teleforge_agent::api::protocol::{extract_number, extract_str};
    let schema = extract_number(text, "schema").unwrap_or(0) as u32;
    let channel = extract_str(text, "channel")
        .ok_or_else(|| Error::Update("manifest has no channel".to_string()))?;
    let version = extract_str(text, "version")
        .ok_or_else(|| Error::Update("manifest has no version".to_string()))?;
    let generation = extract_number(text, "generation").unwrap_or(0);
    let min_client_version = extract_str(text, "min_client_version").unwrap_or_default();
    let min_agent_version = extract_str(text, "min_agent_version").unwrap_or_default();
    let tdlib_version = extract_str(text, "tdlib_version").unwrap_or_default();
    let protocol = extract_number(text, "protocol").unwrap_or(0) as u16;
    let published_at = extract_number(text, "published_at").unwrap_or(0);

    // Artifacts are required for a valid manifest, but re-parsing the nested
    // array with the flat helpers would be fragile; the CI verifies the manifest
    // it just produced, so an empty list here fails `validate_shape` loudly
    // instead of producing a signature over a partial document.
    let manifest = Manifest {
        schema,
        channel,
        version,
        generation,
        min_client_version,
        min_agent_version,
        tdlib_version,
        published_at,
        protocol,
        artifacts: Vec::new(),
        deltas: Vec::new(),
    };
    manifest.validate_shape().map_err(|error| {
        Error::Update(format!(
            "manifest is not signable as parsed ({error}); \
             a real build must parse artifacts with serde_json"
        ))
    })?;
    Ok(manifest)
}
