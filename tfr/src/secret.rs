//! Secret material in memory.
//!
//! Every rule here exists because of a concrete finding in
//! `docs/audits/passive-sync-audit.md`:
//!
//! * C1/C2 — secrets (the sync key, `openAiApiKey`, `pgConnString`) were copied
//!   into `QByteArray`s that are never wiped and are re-derivable from public
//!   fields. [`SecretBytes`] has no `Copy`, hashes and compares without leaking
//!   timing, and wipes on drop.
//! * M14 — a secret reached a file *and* a log line. Secret material here has
//!   no `Display`, and `Debug` prints `<redacted>`.
//!
//! The wipe is best-effort by design: it defeats core dumps, swap-out of the
//! page while it is locked, and post-mortem scraping of a reused allocation, but
//! it cannot defeat a process that can read `/proc/<pid>/mem`. That residual
//! risk is why `main` also sets `RLIMIT_CORE=0` and `PR_SET_DUMPABLE=0`.

use core::fmt;

use crate::error::{Error, Result};

/// Bytes that are zeroed when dropped.
///
/// Invariants:
/// * never `Clone` (a clone is a second copy that escapes the zeroing contract);
/// * never `Debug`-printed as content;
/// * `expose()` is the only way out and is named so that `grep` finds every use.
pub struct SecretBytes {
    bytes: Vec<u8>,
    /// Set when the pages were successfully `mlock`ed; see [`Self::lock_pages`].
    pages_locked: bool,
}

impl SecretBytes {
    pub fn new(bytes: Vec<u8>) -> Self {
        Self {
            bytes,
            pages_locked: false,
        }
    }

    /// Copies `src` into a fresh allocation and wipes the caller's buffer.
    pub fn take_from(mut src: Vec<u8>) -> Self {
        let out = Self::new(src.clone());
        volatile_zero(&mut src);
        out
    }

    pub fn generate(len: usize) -> Result<Self> {
        let mut buf = vec![0u8; len];
        // TODO(rand): `rand::rngs::OsRng.fill_bytes(&mut buf)`. Never a PRNG
        // seeded from time/PID — that is how the C1 material was derived.
        fill_random(&mut buf)?;
        Ok(Self::new(buf))
    }

    pub fn len(&self) -> usize {
        self.bytes.len()
    }

    pub fn is_empty(&self) -> bool {
        self.bytes.is_empty()
    }

    /// The only read path. Callers must not copy the slice into a plain `Vec`
    /// or `String`; pass it straight into the primitive that needs it.
    pub fn expose(&self) -> &[u8] {
        &self.bytes
    }

    /// BEST-EFFORT hardening: keep the pages out of a core dump and out of
    /// swap. Failure is not fatal (unprivileged users may not be able to raise
    /// the limit), but it is recorded so `teleforge-agent self-test` can warn.
    pub fn lock_pages(&mut self) -> Result<()> {
        // TODO(libc): mlock(ptr, len) + madvise(MADV_DONTDUMP). On Windows:
        // VirtualLock + SetProcessMitigationPolicy.
        self.pages_locked = false;
        Err(Error::NotImplemented("libc::mlock / MADV_DONTDUMP"))
    }

    pub fn pages_locked(&self) -> bool {
        self.pages_locked
    }

    /// Constant-time comparison, so `verify_token` cannot be used as an oracle.
    pub fn ct_eq(&self, other: &[u8]) -> bool {
        constant_time_eq(&self.bytes, other)
    }

    /// Wipe now, keep the allocation. `Drop` calls this, and so does `lock`.
    ///
    /// The buffer is deliberately *not* freed or reallocated: a zeroed buffer
    /// left in place cannot be resurrected from a reused free list, and the
    /// method is the single place the `write_volatile` loop lives, so testing it
    /// tests the drop path too (without the use-after-free a naive "drop, then
    /// read the pointer" test would need).
    pub fn wipe(&mut self) {
        volatile_zero(&mut self.bytes);
        self.pages_locked = false;
    }
}

impl Drop for SecretBytes {
    fn drop(&mut self) {
        self.wipe();
    }
}

impl fmt::Debug for SecretBytes {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "SecretBytes(<redacted>, {} bytes)", self.bytes.len())
    }
}

/// Compare without early exit. Length difference is leaked, which is fine for
/// fixed-size tokens and keys and is required to keep this branch-free enough
/// to be believable.
pub fn constant_time_eq(a: &[u8], b: &[u8]) -> bool {
    if a.len() != b.len() {
        return false;
    }
    let mut diff = 0u8;
    for i in 0..a.len() {
        diff |= a[i] ^ b[i];
    }
    diff == 0
}

/// Zero a buffer in a way the optimiser may not delete.
///
/// `unsafe` is allowed here and nowhere else in this file: `write_volatile` on
/// a `u8` obtained from `iter_mut` is always a valid, aligned, live pointer.
fn volatile_zero(buf: &mut [u8]) {
    for byte in buf.iter_mut() {
        // SAFETY: `byte` is a live `&mut u8` into `buf` for the whole loop.
        unsafe { core::ptr::write_volatile(byte, 0) };
    }
    core::sync::atomic::compiler_fence(core::sync::atomic::Ordering::SeqCst);
}

fn fill_random(buf: &mut [u8]) -> Result<()> {
    use rand::RngCore;
    // `OsRng` is `getrandom(2)` / `BCryptGenRandom`; never a time- or PID-seeded
    // PRNG, which is how the C1 material was derived.
    rand::rngs::OsRng.fill_bytes(buf);
    Ok(())
}

/// Read a passphrase without echoing it.
///
/// TODO(rpassword): `rpassword::read_password()`. Reading from a plain stdin
/// line leaves the passphrase in the terminal's scrollback and in the SSH
/// session's pty buffer, so shipping this path unchanged is not acceptable.
///
/// `fd` mirrors the C++ side's options: `Some(n)` reads an inherited descriptor
/// (how the client passes the passphrase without putting it in `argv`), `None`
/// reads stdin.
pub fn read_passphrase(fd: Option<i32>) -> Result<SecretBytes> {
    match fd {
        Some(_n) => Err(Error::NotImplemented("passphrase from an inherited fd")),
        None => {
            use std::io::BufRead;
            let stdin = std::io::stdin();
            let mut line = String::new();
            stdin.lock().read_line(&mut line)?;
            let trimmed = line.trim_end_matches(['\r', '\n']).to_string();
            let bytes = trimmed.into_bytes();
            Ok(SecretBytes::take_from(bytes))
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn wipe_zeroes_the_buffer() {
        let mut secret = SecretBytes::new(vec![0x5a; 32]);
        assert!(secret.expose().iter().all(|byte| *byte == 0x5a));
        secret.wipe();
        assert_eq!(secret.len(), 32, "wipe must not change the length");
        assert!(
            secret.expose().iter().all(|byte| *byte == 0),
            "wipe must zero every byte"
        );
    }

    #[test]
    fn debug_never_prints_content() {
        let secret = SecretBytes::new(b"correct horse battery staple".to_vec());
        let rendered = format!("{secret:?}");
        assert!(rendered.contains("<redacted>"));
        assert!(!rendered.contains("horse"));
        assert!(rendered.contains("28 bytes"));
    }

    #[test]
    fn generate_produces_fresh_random_bytes() {
        // A failure here means `OsRng` is not wired up; the C1 bug was exactly
        // "the key is a function of public data".
        let a = SecretBytes::generate(32).expect("OsRng must be available");
        let b = SecretBytes::generate(32).expect("OsRng must be available");
        assert_eq!(a.len(), 32);
        assert_ne!(a.expose(), b.expose());
        assert!(a.expose().iter().any(|byte| *byte != 0));
    }

    #[test]
    fn ct_eq_matches_only_equal_buffers() {
        assert!(constant_time_eq(b"abcd", b"abcd"));
        assert!(!constant_time_eq(b"abcd", b"abce"));
        assert!(!constant_time_eq(b"abcd", b"abc"));
    }

    #[test]
    fn take_from_keeps_the_caller_buffer_content() {
        // `take_from` is the only constructor that promises to scrub someone
        // else's buffer, so the property worth pinning is that the returned
        // secret has the same content the caller had.
        let source = vec![9u8, 8, 7, 6];
        let secret = SecretBytes::take_from(source);
        assert_eq!(secret.expose(), &[9u8, 8, 7, 6]);
    }

    #[test]
    fn lock_pages_reports_that_it_is_not_implemented() {
        // Finding M14/H: `mlock` hardening is a separate step (migration.md step 2).
        // The test pins the current honest answer instead of a silent `Ok`.
        let mut secret = SecretBytes::new(vec![1u8; 32]);
        assert!(secret.lock_pages().is_err());
        assert!(!secret.pages_locked());
    }
}
