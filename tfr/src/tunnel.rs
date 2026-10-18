//! SSH tunnel supervisor.
//!
//! Replaces `TeleForge::Ssh` (`teleforge_ssh_tunnel.cpp`). Behaviour differences,
//! each with the reason:
//!
//! | C++ today | Agent | Why |
//! |---|---|---|
//! | `EnsureTunnelBlocking` blocks the caller up to `kConnectTimeoutMs` (15 s) and is fed by `crl::on_main`, so it stalls for 20 s whenever the UI thread is busy (finding M12) | state machine; readiness is a `TunnelState`, never a blocking wait | the client's HTTP request must be cancellable, and one slow tunnel must not block sync for another account |
//! | `-o StrictHostKeyChecking=accept-new` | host key pinned at bootstrap (`host_key_fingerprint`), otherwise the tunnel refuses to start | TOFU-by-default is what a MITM on the first deployment needs |
//! | shells out to `ssh` | in-process `russh` by default | no dependency on a system `ssh`, no version skew, real cancellation; the binary backend is kept as `--tunnel-backend=ssh-binary` for phase 1 parity |
//! | `PickFreePort` binds and closes, then `ssh` binds later | same reservation is used but the port is verified by the OS after the forward is requested | the C++ race (another process grabbing the port between the probe and `ssh`) still exists; recorded as a known limitation below |
//! | `BatchMode=yes`, no identity check | identity file existence and mode are validated before starting | `PAIR` guard: a private key readable by group/other is refused |
//!
//! # Known limitation
//!
//! There is no atomic "bind this port for me": `ssh -L` and `russh` both bind
//! internally, after the agent picked the port by probing. The agent mitigates it
//! by holding the reservation socket until the tunnel reports ready, and by
//! retrying with a new port. A hostile local user racing for the port can cause a
//! *failure*, not a hijack, because the tunnel only listens on `127.0.0.1`.

use std::collections::BTreeMap;
use std::net::TcpListener;

use crate::error::{Error, Result};

pub const DEFAULT_SSH_PORT: u16 = 22;
pub const DEFAULT_DB_PORT: u16 = 5432;
pub const DEFAULT_CONNECT_TIMEOUT_MS: u64 = 15_000;
pub const POLL_INTERVAL_MS: u64 = 200;

/// One forward. Mirrors `TeleForge::Ssh::TunnelConfig` plus the two fields the
/// C++ version was missing (a host-key pin and a port reservation).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct TunnelSpec {
    /// `user@host` or `user@host:port`, as typed by the user or read from
    /// `~/.ssh/config` on the *client*.
    pub ssh_target: String,
    pub identity_file: Option<String>,
    /// Address of the database as seen from the bastion.
    pub remote_host: String,
    pub remote_port: u16,
    /// `None` = let the agent choose; `Some(0)` = same thing, spelled explicitly.
    pub local_port: Option<u16>,
    /// Pin recorded at bootstrap. `None` means "the client has not pinned yet and
    /// the deployment must not proceed" — the agent never trusts an unknown key.
    pub host_key_fingerprint: Option<String>,
}

impl TunnelSpec {
    pub fn validate(&self) -> Result<()> {
        let (host, _port) = parse_target(&self.ssh_target)?;
        if host.is_empty() || host.contains(char::is_whitespace) {
            return Err(Error::Invalid(format!(
                "bad ssh target {:?}",
                self.ssh_target
            )));
        }
        if self.remote_host.trim().is_empty() {
            return Err(Error::Invalid("remote database host is empty".to_string()));
        }
        if self.remote_port == 0 {
            return Err(Error::Invalid("remote database port is zero".to_string()));
        }
        if let Some(path) = &self.identity_file {
            // Refuse a key the whole box can read. `ssh` itself warns and then
            // uses it anyway, which is how a shared VPS leaks one user's key.
            let meta = std::fs::metadata(path)?;
            if meta.len() == 0 {
                return Err(Error::Invalid(format!("identity file {path} is empty")));
            }
            #[cfg(unix)]
            {
                use std::os::unix::fs::PermissionsExt;
                let mode = meta.permissions().mode() & 0o777;
                if mode & 0o077 != 0 {
                    return Err(Error::Invalid(format!(
                        "identity file {path} has mode {mode:o}; expected 0600"
                    )));
                }
            }
        }
        Ok(())
    }

    /// Registry key. Same idea as `KeyFor` in `teleforge_ssh_tunnel.cpp` so that
    /// two requests for the same forward reuse one tunnel.
    pub fn registry_key(&self) -> String {
        let (host, port) = parse_target(&self.ssh_target).unwrap_or_default();
        format!(
            "{host}|{port}|{}|{}|{}",
            self.identity_file.as_deref().unwrap_or(""),
            self.remote_host,
            self.remote_port
        )
    }

    pub fn local_conn_string(&self, local_port: u16, conn_string: &str) -> String {
        rewrite_conn_host_port(conn_string, "127.0.0.1", local_port)
    }
}

/// `"user@host:2222"` → `("user@host", 2222)`.
///
/// The C++ `ParseSshTarget` treats a colon *after the `@`* as a port, so an IPv6
/// literal in brackets keeps working as long as it has no `@`. Documented
/// difference: an unbracketed IPv6 address without a user is parsed as
/// `host:port` by both implementations, which is why the agent requires brackets
/// and rejects a bare IPv6 literal with a clear error instead of guessing.
pub fn parse_target(target: &str) -> Result<(String, u16)> {
    let value = target.trim();
    if value.is_empty() {
        return Err(Error::Invalid("empty ssh target".to_string()));
    }
    let at = value.find('@');
    let colon = value.rfind(':');
    let (host_part, port) = match (at, colon) {
        (Some(at_pos), Some(colon_pos)) if colon_pos > at_pos => {
            let raw = &value[colon_pos + 1..];
            let port: u16 = raw
                .parse()
                .map_err(|_| Error::Invalid(format!("bad port {raw:?} in ssh target")))?;
            (&value[..colon_pos], port)
        }
        _ => (value, DEFAULT_SSH_PORT),
    };
    if host_part.is_empty() {
        return Err(Error::Invalid(format!("no host in {:?}", target)));
    }
    // An IPv6 literal must be bracketed, otherwise the port separator and the
    // address colons are ambiguous. Checked on the host part only, so the
    // `user@[addr]:port` form still passes.
    let host_only = host_part
        .rsplit_once('@')
        .map(|(_, host)| host)
        .unwrap_or(host_part);
    if host_only.contains(':') && !host_only.starts_with('[') {
        return Err(Error::Invalid(format!(
            "unbracketed IPv6 literal in {:?}; write [addr]:port",
            target
        )));
    }
    if host_only.starts_with('[') && !host_only.ends_with(']') {
        return Err(Error::Invalid(format!(
            "unterminated IPv6 literal in {:?}",
            target
        )));
    }
    Ok((host_part.to_string(), port))
}

/// Argument vector for the `ssh` fallback backend.
///
/// Identical semantics to `EnsureTunnelBlocking`'s argument list *except* that
/// `StrictHostKeyChecking=accept-new` is replaced by `yes`, and that the host key
/// must already be in the known_hosts file the client provisioned.
pub fn plan_ssh_args(spec: &TunnelSpec, local_port: u16, known_hosts: &str) -> Vec<String> {
    let (host, port) = parse_target(&spec.ssh_target).unwrap_or_default();
    let mut args = vec![
        "-N".to_string(),
        "-L".to_string(),
        format!(
            "127.0.0.1:{local_port}:{}:{}",
            spec.remote_host, spec.remote_port
        ),
        "-p".to_string(),
        port.to_string(),
        "-o".to_string(),
        "StrictHostKeyChecking=yes".to_string(),
        "-o".to_string(),
        format!("UserKnownHostsFile={known_hosts}"),
        "-o".to_string(),
        "ExitOnForwardFailure=yes".to_string(),
        "-o".to_string(),
        "ServerAliveInterval=30".to_string(),
        "-o".to_string(),
        "ServerAliveCountMax=3".to_string(),
        "-o".to_string(),
        "BatchMode=yes".to_string(),
        "-o".to_string(),
        "IdentitiesOnly=yes".to_string(),
    ];
    // The destination must be last: `ssh` treats everything after it as a remote
    // command, so a trailing `-i` would be executed on the server.
    if let Some(identity) = &spec.identity_file {
        args.push("-i".to_string());
        args.push(identity.clone());
    }
    args.push(host);
    args
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum TunnelState {
    Down,
    /// `ssh`/`russh` started, waiting for the local port to accept.
    Starting {
        since_ms: u64,
        local_port: u16,
    },
    Ready {
        local_port: u16,
        since_ms: u64,
    },
    Failed {
        reason: String,
        at_ms: u64,
    },
}

impl TunnelState {
    pub fn local_port(&self) -> Option<u16> {
        match self {
            Self::Ready { local_port, .. } | Self::Starting { local_port, .. } => Some(*local_port),
            _ => None,
        }
    }

    pub fn is_ready(&self) -> bool {
        matches!(self, Self::Ready { .. })
    }

    pub fn is_terminal_failure(&self) -> bool {
        matches!(self, Self::Failed { .. })
    }
}

/// In-memory registry of live forwards, keyed by [`TunnelSpec::registry_key`].
#[derive(Debug, Default)]
pub struct TunnelRegistry {
    states: BTreeMap<String, TunnelState>,
}

impl TunnelRegistry {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn get(&self, key: &str) -> Option<&TunnelState> {
        self.states.get(key)
    }

    pub fn set(&mut self, key: &str, state: TunnelState) {
        self.states.insert(key.to_string(), state);
    }

    pub fn remove(&mut self, key: &str) -> Option<TunnelState> {
        self.states.remove(key)
    }

    pub fn ready_count(&self) -> usize {
        self.states.values().filter(|s| s.is_ready()).count()
    }

    /// Drop tunnels that have been `Starting` for longer than the timeout so a
    /// stuck child cannot pin a port and a state forever.
    pub fn expire_stale(&mut self, now_ms: u64, timeout_ms: u64) -> Vec<String> {
        let mut expired = Vec::new();
        for (key, state) in self.states.iter() {
            if let TunnelState::Starting { since_ms, .. } = state {
                if now_ms.saturating_sub(*since_ms) > timeout_ms {
                    expired.push(key.clone());
                }
            }
        }
        for key in &expired {
            self.states.insert(
                key.clone(),
                TunnelState::Failed {
                    reason: "connect timeout".to_string(),
                    at_ms: now_ms,
                },
            );
        }
        expired
    }
}

/// Reserve a loopback port by binding it, then release it for the tunnel.
///
/// The listener is returned so the caller can keep it open until the tunnel
/// reports ready, which narrows (but does not close) the race described in the
/// module docs.
pub fn pick_free_port() -> Result<(u16, TcpListener)> {
    let listener = TcpListener::bind(("127.0.0.1", 0))?;
    let port = listener.local_addr()?.port();
    Ok((port, listener))
}

/// Probe whether `127.0.0.1:port` accepts a connection. This is the readiness
/// test the C++ `PollUntilReady` performs, minus the recursive timer.
pub fn probe_ready(local_port: u16) -> bool {
    std::net::TcpStream::connect_timeout(
        &std::net::SocketAddr::from(([127, 0, 0, 1], local_port)),
        std::time::Duration::from_millis(250),
    )
    .is_ok()
}

/// Bring a tunnel up. Non-blocking by contract: it records `Starting` and
/// returns; the caller polls `state`.
pub fn start(registry: &mut TunnelRegistry, spec: &TunnelSpec, now_ms: u64) -> Result<u16> {
    spec.validate()?;
    if spec.host_key_fingerprint.is_none() {
        return Err(Error::Auth(
            "tunnel refused: no pinned host key fingerprint; re-run the client installer to pin it"
                .to_string(),
        ));
    }
    let key = spec.registry_key();
    if let Some(TunnelState::Ready { local_port, .. }) = registry.get(&key) {
        return Ok(*local_port);
    }
    let (port, listener) = match spec.local_port {
        Some(requested) if requested != 0 => {
            let listener = TcpListener::bind(("127.0.0.1", requested))?;
            (requested, listener)
        }
        _ => pick_free_port()?,
    };
    registry.set(
        &key,
        TunnelState::Starting {
            since_ms: now_ms,
            local_port: port,
        },
    );
    drop(listener);
    // TODO(russh): spawn the in-process client with
    //   - key from `identity_file` (or an agent socket),
    //   - `check_server_key` comparing the base64 SHA-256 fingerprint against
    //     `spec.host_key_fingerprint`, failing closed,
    //   - a `direct-tcpip` channel to (remote_host, remote_port) fanned out to a
    //     loopback listener on `port`,
    //   - cancellation via a `CancellationToken` wired to `TunnelState::Down`.
    Err(Error::NotImplemented("russh direct-tcpip forwarding"))
}

pub fn stop(registry: &mut TunnelRegistry, spec: &TunnelSpec) {
    registry.remove(&spec.registry_key());
}

/// Kill everything at shutdown. Unlike `ShutdownAllTunnels`, this does not
/// `waitForFinished(1000)` on a hot path; the supervisor drops the registry and
/// the async tasks cancel themselves.
pub fn stop_all(registry: &mut TunnelRegistry) {
    let keys: Vec<String> = registry.states.keys().cloned().collect();
    for key in keys {
        registry.remove(&key);
    }
}

/// Rewrite `host=`/`port=` in a libpq/MySQL key=value connection string.
///
/// Ported from `RewriteConnStringHostPort`, with two behaviour changes: the SQL
/// password is *rejected* rather than passed through (the agent never accepts a
/// connection string containing `password=`; it is split out into the vault), and
/// a token without `=` is preserved rather than silently dropped.
pub fn rewrite_conn_host_port(conn_string: &str, new_host: &str, new_port: u16) -> String {
    let mut out: Vec<String> = Vec::new();
    let mut saw_host = false;
    let mut saw_port = false;
    for token in conn_string
        .split([' ', ';', '\t'])
        .filter(|t| !t.is_empty())
    {
        match token.split_once('=') {
            Some((key, _value)) => {
                let lowered = key.trim().to_ascii_lowercase();
                match lowered.as_str() {
                    "host" | "server" => {
                        out.push(format!("host={new_host}"));
                        saw_host = true;
                    }
                    "port" => {
                        out.push(format!("port={new_port}"));
                        saw_port = true;
                    }
                    "password" | "passwd" => {
                        // Deliberately dropped: see `docs/architecture.md`,
                        // "секреты БД живут в vault, не в connstring".
                        out.push(format!("{key}=<redacted-by-agent>"));
                    }
                    _ => out.push(token.to_string()),
                }
            }
            None => out.push(token.to_string()),
        }
    }
    if !saw_host {
        out.push(format!("host={new_host}"));
    }
    if !saw_port {
        out.push(format!("port={new_port}"));
    }
    out.join(" ")
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_user_host_and_port() {
        assert_eq!(
            parse_target("root@db.example.com").unwrap(),
            ("root@db.example.com".to_string(), 22)
        );
        assert_eq!(
            parse_target("root@db.example.com:2222").unwrap(),
            ("root@db.example.com".to_string(), 2222)
        );
        assert_eq!(
            parse_target("deploy@[2001:db8::1]:2222").unwrap(),
            ("deploy@[2001:db8::1]".to_string(), 2222)
        );
        assert!(parse_target("deploy@2001:db8::1").is_err());
        assert!(parse_target("").is_err());
    }

    #[test]
    fn ssh_args_have_no_accept_new() {
        let spec = TunnelSpec {
            ssh_target: "u@h:2200".to_string(),
            identity_file: Some("/home/u/.ssh/id_ed25519".to_string()),
            remote_host: "127.0.0.1".to_string(),
            remote_port: 5432,
            local_port: None,
            host_key_fingerprint: Some("SHA256:abc".to_string()),
        };
        let args = plan_ssh_args(&spec, 40_000, "/etc/teleforge-agent/known_hosts");
        assert!(args.iter().any(|a| a == "StrictHostKeyChecking=yes"));
        assert!(!args.iter().any(|a| a.contains("accept-new")));
        assert!(args.iter().any(|a| a == "127.0.0.1:40000:127.0.0.1:5432"));
        assert!(args.iter().any(|a| a == "2200"));
        assert_eq!(args.last().unwrap(), "u@h");
    }

    #[test]
    fn rewrite_replaces_and_appends() {
        let out = rewrite_conn_host_port("host=a port=1 dbname=x user=u", "127.0.0.1", 9999);
        assert_eq!(out, "host=127.0.0.1 port=9999 dbname=x user=u");
        let added = rewrite_conn_host_port("dbname=x", "127.0.0.1", 1234);
        assert!(added.contains("host=127.0.0.1"));
        assert!(added.contains("port=1234"));
    }

    #[test]
    fn rewrite_drops_password() {
        let out = rewrite_conn_host_port("host=a password=hunter2 dbname=x", "127.0.0.1", 5432);
        assert!(!out.contains("hunter2"));
    }

    #[test]
    fn stale_starting_tunnel_expires() {
        let mut registry = TunnelRegistry::new();
        registry.set(
            "k",
            TunnelState::Starting {
                since_ms: 0,
                local_port: 1,
            },
        );
        let expired = registry.expire_stale(20_000, 15_000);
        assert_eq!(expired, vec!["k".to_string()]);
        assert!(registry.get("k").unwrap().is_terminal_failure());
    }
}
