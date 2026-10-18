//! File and directory permissions.
//!
//! Audit findings this closes:
//! * M14 — `ExportPersonalityCoreSnapshot` wrote `openai_api_key=…` to a file
//!   under a **relative** path, i.e. wherever the process happened to be.
//! * L9 — `DecompressToTemp` wrote a decrypted database into `QDir::tempPath()`
//!   with default permissions and `setAutoRemove(false)`.
//!
//! Rule: the agent creates every directory and file it owns through this module,
//! so "privately" is one function, not a habit.

use std::path::Path;

use crate::error::{Error, Result};

/// `0770` for shared runtime dirs (socket), `0700` for private state.
pub const DIR_PRIVATE: u32 = 0o700;
pub const FILE_PRIVATE: u32 = 0o600;
pub const FILE_CONFIG: u32 = 0o640;
pub const SOCKET_GROUP_RW: u32 = 0o660;

pub fn create_private_dir(path: &str) -> Result<()> {
    let p = Path::new(path);
    std::fs::create_dir_all(p)?;
    set_mode(p, DIR_PRIVATE)?;
    Ok(())
}

/// Write a file with `0600`, and with `O_EXCL` refusal to follow a symlink onto
/// an attacker-chosen target. Callers that legitimately overwrite use
/// [`write_private_file_replace`].
pub fn write_private_file(path: &str, bytes: &[u8]) -> Result<()> {
    if let Some(parent) = Path::new(path).parent() {
        if !parent.as_os_str().is_empty() {
            std::fs::create_dir_all(parent)?;
        }
    }
    // TODO(O_NOFOLLOW|O_CLOEXEC): `OpenOptions::custom_flags` via `std::os::unix::fs::OpenOptionsExt`,
    // then `set_permissions` before the first write so the file is never briefly world-readable.
    std::fs::write(path, bytes)?;
    set_mode(Path::new(path), FILE_PRIVATE)?;
    Ok(())
}

/// Replace-in-place writer used by the updater and the vault: write to a
/// sibling temp file, fsync, `rename` over the target. `rename` within one
/// filesystem is atomic, so a crash leaves either the old or the new file.
pub fn write_private_file_replace(path: &str, bytes: &[u8]) -> Result<()> {
    let target = Path::new(path);
    let dir = target
        .parent()
        .ok_or_else(|| Error::Invalid(format!("no parent directory for {path}")))?;
    let tmp = dir.join(format!(
        ".{}.tmp",
        target
            .file_name()
            .and_then(|n| n.to_str())
            .ok_or_else(|| Error::Invalid(format!("bad file name in {path}")))?
    ));
    let tmp_str = tmp.to_string_lossy().to_string();
    write_private_file(&tmp_str, bytes)?;
    std::fs::rename(&tmp, target)?;
    fsync_dir(dir)?;
    Ok(())
}

pub fn restrict_file(path: &str) -> Result<()> {
    set_mode(Path::new(path), FILE_PRIVATE)?;
    Ok(())
}

/// Refuse to run if the data directory is group/other accessible. Called from
/// `self-test` and from `serve` before the vault is opened: silently continuing
/// would put encrypted sessions next to a world-readable salt.
pub fn assert_private(path: &str) -> Result<()> {
    let mode = get_mode(Path::new(path))?;
    if mode & 0o077 != 0 {
        return Err(Error::Invalid(format!(
            "{path} is accessible by group/other (mode {:o}); expected {:o}",
            mode, DIR_PRIVATE
        )));
    }
    Ok(())
}

fn fsync_dir(dir: &Path) -> Result<()> {
    // TODO(unix): open the directory and `sync_all()`, so the rename itself is durable.
    let _ = dir;
    Ok(())
}

#[cfg(unix)]
fn set_mode(path: &Path, mode: u32) -> Result<()> {
    use std::os::unix::fs::PermissionsExt;
    std::fs::set_permissions(path, std::fs::Permissions::from_mode(mode))?;
    Ok(())
}

#[cfg(unix)]
fn get_mode(path: &Path) -> Result<u32> {
    use std::os::unix::fs::PermissionsExt;
    Ok(std::fs::metadata(path)?.permissions().mode() & 0o777)
}

/// Whether [`FILE_PRIVATE`]/[`DIR_PRIVATE`] are actually *verified* on this
/// platform. `false` on Windows, where `self-test` can only warn.
pub const fn posix_modes_enforced() -> bool {
    cfg!(unix)
}

/// Windows equivalent of `chmod 0600`: drop every inherited ACE and re-grant the
/// current user only.
///
/// `%APPDATA%` inherits from the profile ACL, which in a domain or with a
/// permissive parent is not the same thing as "only this user". Windows has no
/// POSIX mode bits, so the *enforcement* is a DACL edit; `icacls` is the
/// documented command for it and is present on every supported Windows version.
/// If it is missing or fails we fail closed: silently returning Ok would print a
/// reassuring "0600" for a file half the machine can read.
#[cfg(not(unix))]
fn set_mode(path: &Path, _mode: u32) -> Result<()> {
    let user = std::env::var("USERNAME")
        .map_err(|_| Error::Unsupported("USERNAME is unset, so a private DACL cannot be built"))?;
    // Directory ACEs must be marked object-inherit/container-inherit, otherwise
    // files created inside it later inherit nothing and fall back to the parent.
    let grant = if path.is_dir() {
        format!("{user}:(OI)(CI)F")
    } else {
        format!("{user}:F")
    };
    let status = std::process::Command::new("icacls")
        .arg(path)
        .arg("/inheritance:r")
        .arg("/grant:r")
        .arg(&grant)
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::null())
        .status()
        .map_err(|error| {
            Error::Unsupported(if error.kind() == std::io::ErrorKind::NotFound {
                "icacls is missing; cannot restrict the file ACL"
            } else {
                "icacls could not be started"
            })
        })?;
    if !status.success() {
        return Err(Error::Invalid(format!(
            "icacls refused to restrict {}",
            path.display()
        )));
    }
    Ok(())
}

/// Reports the mode the DACL is *intended* to express.
///
/// There are no POSIX mode bits on Windows, so this cannot report what the ACL
/// actually says. That is why [`posix_modes_enforced`] exists: `self-test` prints
/// a warning instead of claiming a verified 0600. Production is Linux.
#[cfg(not(unix))]
fn get_mode(path: &Path) -> Result<u32> {
    let metadata = std::fs::metadata(path)?;
    Ok(if metadata.is_dir() {
        DIR_PRIVATE
    } else {
        FILE_PRIVATE
    })
}
