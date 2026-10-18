//! Test-only helpers.
//!
//! # Why not `tempfile::tempdir()` directly
//!
//! On a locked-down Windows host the OS temp directory can be writable for the
//! interactive shell but not for a spawned test process (a restricted token with a
//! different ACL). Rather than making every test platform-aware, [`TestDir`] tries
//! the OS temp directory first and falls back to `target/test-tmp` inside the crate
//! — which is always writable, because it is where the build already writes.
//!
//! On a normal Linux box the first candidate wins and this is exactly a temp
//! directory.

use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};

static COUNTER: AtomicU64 = AtomicU64::new(0);

/// A directory that removes itself on drop.
pub struct TestDir {
    path: PathBuf,
}

impl TestDir {
    pub fn new(label: &str) -> Self {
        let unique = format!(
            "{label}-{}-{}",
            std::process::id(),
            COUNTER.fetch_add(1, Ordering::Relaxed)
        );
        let roots = [
            std::env::temp_dir().join("teleforge-agent-tests"),
            Path::new(env!("CARGO_MANIFEST_DIR")).join("target/test-tmp"),
        ];
        for root in roots {
            let path = root.join(&unique);
            match std::fs::create_dir_all(&path) {
                Ok(()) => return Self { path },
                Err(error) => {
                    eprintln!(
                        "test_support: cannot use {} ({error}); trying the next root",
                        path.display()
                    );
                }
            }
        }
        panic!("test_support: no writable directory for the test store");
    }

    pub fn path(&self) -> &Path {
        &self.path
    }

    pub fn join(&self, name: &str) -> PathBuf {
        self.path.join(name)
    }
}

impl Drop for TestDir {
    fn drop(&mut self) {
        // Best effort: a still-open SQLite handle keeps `-wal`/`-shm` alive on
        // Windows, and leaving a directory behind must never fail a test.
        let _ = std::fs::remove_dir_all(&self.path);
    }
}

impl core::fmt::Debug for TestDir {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        f.debug_struct("TestDir").field("path", &self.path).finish()
    }
}
