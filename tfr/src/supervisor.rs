//! Component supervisor: who owns what, and what happens when one part dies.
//!
//! The C++ side has no supervisor: `EnsureTunnelBlocking` is called from inside
//! whatever worker needs a connection, the TDLib session belongs to the UI
//! process, and `ShutdownAllTunnels` is a fire-and-forget `crl::on_main` at exit.
//! The result is the "no cancellation, no backoff, no owner" pattern behind
//! findings M12 and H2.
//!
//! Here every long-lived thing is a [`Component`] with an explicit state, one
//! owner, and a restart policy. The agent is deployed as a user-level systemd
//! unit, so even a total agent crash is recovered by systemd — the supervisor only
//! has to handle *partial* failure without the whole process exiting.

use crate::error::{Error, Result};

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub enum Component {
    /// TLS/unix-socket listener.
    Api,
    /// Vault state; can be `Locked` without being `Failed`.
    Vault,
    /// One SSH forward per database target.
    Tunnel,
    /// One TDLib instance per Telegram account.
    Tdlib,
    /// Sync worker (single owner of the SQLite store).
    Sync,
    /// Manifest check + staged install.
    Updater,
}

impl Component {
    pub fn as_str(self) -> &'static str {
        match self {
            Self::Api => "api",
            Self::Vault => "vault",
            Self::Tunnel => "tunnel",
            Self::Tdlib => "tdlib",
            Self::Sync => "sync",
            Self::Updater => "updater",
        }
    }

    /// Whether a failure of this component must stop the agent.
    ///
    /// Only the API is fatal: without it the agent is unreachable and an operator
    /// has to look at it. Everything else degrades — a Telegram account that fails
    /// to start must not take down DB sync for another account.
    pub fn failure_is_fatal(self) -> bool {
        matches!(self, Self::Api)
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum State {
    Starting,
    Running,
    /// Intentionally stopped (vault locked, account logged out).
    Idle,
    Failed {
        reason: String,
        since_ms: u64,
        attempts: u32,
    },
    Stopped,
}

impl State {
    pub fn is_running(&self) -> bool {
        matches!(self, Self::Running)
    }

    pub fn is_failed(&self) -> bool {
        matches!(self, Self::Failed { .. })
    }
}

/// Exponential backoff with a cap.
///
/// The absence of this is finding H2: on `FLOOD_WAIT` the C++ scheduler simply
/// ticked again 5 seconds later, and `PollUserOnline` had no `.fail()` at all. Here
/// a failing component must go through [`Backoff`] before its next attempt, so a
/// failing Telegram account *provably* cannot hammer the server.
#[derive(Debug)]
pub struct Backoff {
    base_ms: u64,
    max_ms: u64,
    attempt: u32,
}

impl Backoff {
    pub fn new(base_ms: u64, max_ms: u64) -> Self {
        Self {
            base_ms: base_ms.max(1),
            max_ms: max_ms.max(base_ms),
            attempt: 0,
        }
    }

    pub fn attempts(&self) -> u32 {
        self.attempt
    }

    /// Delay before the next attempt, and consume one attempt.
    ///
    /// Saturating arithmetic: `base << 20` overflows instantly on a long outage,
    /// and an overflow here would produce a *tiny* delay — the opposite of what a
    /// backoff is for.
    pub fn next_delay_ms(&mut self) -> u64 {
        let shift = self.attempt.min(31);
        let scaled = self.base_ms.saturating_mul(1u64 << shift);
        self.attempt = self.attempt.saturating_add(1);
        scaled.min(self.max_ms)
    }

    pub fn reset(&mut self) {
        self.attempt = 0;
    }

    /// Total wait across `n` attempts, used by the self-test to assert the policy.
    pub fn total_delay_ms(&mut self, attempts: u32) -> u64 {
        let mut total = 0u64;
        for _ in 0..attempts {
            total = total.saturating_add(self.next_delay_ms());
        }
        total
    }
}

/// Per-component bookkeeping.
#[derive(Debug)]
pub struct ComponentSlot {
    pub component: Component,
    pub state: State,
    pub backoff: Backoff,
    pub last_error: Option<String>,
    pub restarts: u32,
}

impl ComponentSlot {
    pub fn new(component: Component) -> Self {
        Self {
            component,
            state: State::Starting,
            backoff: Backoff::new(500, 60_000),
            last_error: None,
            restarts: 0,
        }
    }

    pub fn mark_running(&mut self) {
        self.state = State::Running;
        self.backoff.reset();
        self.last_error = None;
    }

    pub fn mark_idle(&mut self) {
        self.state = State::Idle;
        self.backoff.reset();
    }

    /// Record a failure and decide whether it is fatal.
    pub fn fail(&mut self, reason: &str, now_ms: u64) -> Result<()> {
        self.restarts = self.restarts.saturating_add(1);
        self.last_error = Some(reason.to_string());
        if self.component.failure_is_fatal() {
            self.state = State::Failed {
                reason: reason.to_string(),
                since_ms: now_ms,
                attempts: self.backoff.attempts(),
            };
            return Err(Error::Protocol(format!(
                "fatal component {} failed: {reason}",
                self.component.as_str()
            )));
        }
        self.state = State::Failed {
            reason: reason.to_string(),
            since_ms: now_ms,
            attempts: self.backoff.attempts(),
        };
        Ok(())
    }

    pub fn next_retry_in_ms(&mut self) -> u64 {
        self.backoff.next_delay_ms()
    }
}

/// The whole agent's component table.
#[derive(Debug)]
pub struct Supervisor {
    pub slots: Vec<ComponentSlot>,
    pub started_at_ms: u64,
}

impl Supervisor {
    pub fn new(now_ms: u64) -> Self {
        let slots = [
            Component::Api,
            Component::Vault,
            Component::Tunnel,
            Component::Tdlib,
            Component::Sync,
            Component::Updater,
        ]
        .into_iter()
        .map(ComponentSlot::new)
        .collect();
        Self {
            slots,
            started_at_ms: now_ms,
        }
    }

    pub fn slot(&mut self, component: Component) -> &mut ComponentSlot {
        // The table is built from the enum itself, so this cannot miss.
        for slot in self.slots.iter_mut() {
            if slot.component == component {
                return slot;
            }
        }
        unreachable!("component table is exhaustive")
    }

    /// Health, in the shape `status` returns and the client's UI renders.
    pub fn health_json(&self) -> String {
        let items: Vec<String> = self
            .slots
            .iter()
            .map(|slot| {
                let state = match &slot.state {
                    State::Starting => "starting".to_string(),
                    State::Running => "running".to_string(),
                    State::Idle => "idle".to_string(),
                    State::Stopped => "stopped".to_string(),
                    State::Failed {
                        since_ms, attempts, ..
                    } => format!("failed(attempts={attempts},since={since_ms})"),
                };
                let error = match &slot.last_error {
                    Some(reason) => format!("\"{}\"", crate::api::protocol::json_escape(reason)),
                    None => "null".to_string(),
                };
                format!(
                    "\"{}\":{{\"state\":\"{state}\",\"restarts\":{},\"last_error\":{error}}}",
                    slot.component.as_str(),
                    slot.restarts
                )
            })
            .collect();
        format!("{{{}}}", items.join(","))
    }

    /// True when every component is either running or intentionally idle.
    pub fn is_healthy(&self) -> bool {
        self.slots
            .iter()
            .all(|slot| matches!(slot.state, State::Running | State::Idle))
    }

    /// Exit code for `self-test`, so the client's installer can gate on it.
    pub fn self_test_exit_code(&self) -> i32 {
        if self.is_healthy() {
            crate::error::exit::OK
        } else {
            crate::error::exit::SELF_TEST_FAILED
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn backoff_grows_and_caps() {
        let mut backoff = Backoff::new(500, 4_000);
        assert_eq!(backoff.next_delay_ms(), 500);
        assert_eq!(backoff.next_delay_ms(), 1_000);
        assert_eq!(backoff.next_delay_ms(), 2_000);
        assert_eq!(backoff.next_delay_ms(), 4_000);
        assert_eq!(backoff.next_delay_ms(), 4_000);
    }

    #[test]
    fn backoff_does_not_overflow_into_a_tiny_delay() {
        let mut backoff = Backoff::new(1_000, 60_000);
        for _ in 0..64 {
            let delay = backoff.next_delay_ms();
            assert!(
                (1_000..=60_000).contains(&delay),
                "delay {delay} out of range"
            );
        }
    }

    #[test]
    fn backoff_resets_after_success() {
        let mut backoff = Backoff::new(100, 1_000);
        assert_eq!(backoff.next_delay_ms(), 100);
        assert_eq!(backoff.next_delay_ms(), 200);
        backoff.reset();
        assert_eq!(backoff.next_delay_ms(), 100);
    }

    #[test]
    fn api_failure_is_fatal_others_are_not() {
        let mut supervisor = Supervisor::new(0);
        assert!(supervisor
            .slot(Component::Api)
            .fail("port busy", 1)
            .is_err());
        assert!(supervisor
            .slot(Component::Tdlib)
            .fail("flood wait", 1)
            .is_ok());
        assert!(supervisor.slot(Component::Api).state.is_failed());
        assert!(supervisor.slot(Component::Tdlib).state.is_failed());
        assert!(!supervisor.is_healthy());
    }

    #[test]
    fn health_returns_to_green_after_recovery() {
        let mut supervisor = Supervisor::new(0);
        supervisor.slot(Component::Sync).fail("busy", 1).ok();
        assert!(!supervisor.is_healthy());
        supervisor.slot(Component::Sync).mark_running();
        for slot in supervisor.slots.iter_mut() {
            slot.mark_running();
        }
        assert!(supervisor.is_healthy());
        assert_eq!(supervisor.self_test_exit_code(), 0);
    }

    #[test]
    fn health_json_lists_every_component() {
        let mut supervisor = Supervisor::new(0);
        supervisor.slot(Component::Api).mark_running();
        let json = supervisor.health_json();
        for component in ["api", "vault", "tunnel", "tdlib", "sync", "updater"] {
            assert!(json.contains(component), "{component} missing from {json}");
        }
        assert!(json.starts_with('{') && json.ends_with('}'));
    }
}
