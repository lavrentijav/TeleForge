//! Signal handling: turn SIGTERM/SIGINT into a flag the accept loop observes.
//!
//! # Why this is `unsafe` and in its own module
//!
//! The crate confines `unsafe` to [`crate::secret`], [`crate::tdlib`] and this
//! module. Here it is exactly one `libc::signal` call per signal, with a handler that
//! does one thing: `AtomicBool::store`. That is async-signal-safe (no allocation, no
//! locks, no `eprintln`), which is the only reason a handler may do anything at all.
//!
//! # Why a static flag and not a channel
//!
//! A channel would need the sender to be reachable from a `extern "C"` function,
//! i.e. a global holding a locked pointer — more `unsafe` than a single atomic. The
//! accept loop polls the flag every ~20 ms (it already polls for shutdown), so the
//! cost is invisible and the shutdown is still orderly: the listener stops accepting,
//! the retention thread finishes its current slice, and open stores are dropped (which
//! checkpoints WAL).
//!
//! # What this is not
//!
//! It is not a `SIGKILL` story (impossible to catch) and not a `SIGHUP` reload. The
//! systemd unit's `TimeoutStopSec=20` is the out; in-flight requests are short
//! (`sync.push` of 2000 events is one transaction).
//!
//! Windows has no `SIGTERM`; there the `shutdown` RPC and the process being killed are
//! the whole story, which is why the non-unix implementation is a no-op rather than a
//! console-control handler — the production target is Linux.

#[cfg(unix)]
use std::sync::atomic::{AtomicBool, Ordering};

#[cfg(unix)]
static SHUTDOWN: AtomicBool = AtomicBool::new(false);

/// Install handlers for `SIGTERM` and `SIGINT`. Idempotent; the second call is a
/// no-op because the handler is the same function pointer.
#[cfg(unix)]
pub fn install() {
    extern "C" fn on_signal(_signal: libc::c_int) {
        SHUTDOWN.store(true, Ordering::SeqCst);
    }
    // SAFETY: `signal(2)` with a handler that only touches an atomic is the
    // documented async-signal-safe subset. The casts are the traditional
    // `sighandler_t` representation in `libc`.
    unsafe {
        libc::signal(libc::SIGTERM, on_signal as libc::sighandler_t);
        libc::signal(libc::SIGINT, on_signal as libc::sighandler_t);
    }
}

/// Whether a termination signal has been received.
#[cfg(unix)]
pub fn triggered() -> bool {
    SHUTDOWN.load(Ordering::SeqCst)
}

/// No-op: see the module docs.
#[cfg(not(unix))]
pub fn install() {}

/// No-op: see the module docs.
#[cfg(not(unix))]
pub fn triggered() -> bool {
    false
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn installing_without_a_signal_does_not_trigger() {
        install();
        // Only meaningful on unix; on Windows it is a no-op by design.
        if !cfg!(unix) {
            assert!(!triggered());
        }
    }
}
