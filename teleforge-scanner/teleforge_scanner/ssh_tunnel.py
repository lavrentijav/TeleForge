"""Optional SSH tunnel for reaching the cloud DB without exposing its port.

Mirrors the desktop TeleForge client's SSH-tunnel support (which shells out to
the system `ssh -L`): here we use the `sshtunnel` package (paramiko-based) so
the scanner stays a pure-Python install with no system `ssh` binary required.
"""

from __future__ import annotations

import logging
import re

from .config import Config, SshTunnelConfig

log = logging.getLogger("teleforge.ssh_tunnel")

_HOST_PORT_RE = re.compile(r"\b(host|server|port)\s*=\s*(\S+)", re.IGNORECASE)


def _parse_ssh_target(target: str) -> tuple[str, str, int]:
    """"user@host" or "user@host:port" -> (user, host, port)."""
    value = target.strip()
    user = ""
    if "@" in value:
        user, _, value = value.partition("@")
    host, _, port_s = value.partition(":")
    port = int(port_s) if port_s else 22
    return user, host, port


def extract_dsn_host_port(dsn: str, default_port: int) -> tuple[str, int]:
    """Reads host=/port= out of a libpq-style `key=value` connection string."""
    host = "127.0.0.1"
    port = default_port
    for key, value in _HOST_PORT_RE.findall(dsn):
        if key.lower() in ("host", "server"):
            host = value
        elif key.lower() == "port":
            try:
                port = int(value)
            except ValueError:
                pass
    return host, port


def rewrite_dsn_host_port(dsn: str, new_host: str, new_port: int) -> str:
    seen_host = False
    seen_port = False

    def repl(m: re.Match) -> str:
        nonlocal seen_host, seen_port
        key = m.group(1).lower()
        if key in ("host", "server"):
            seen_host = True
            return f"host={new_host}"
        seen_port = True
        return f"port={new_port}"

    out = _HOST_PORT_RE.sub(repl, dsn)
    if not seen_host:
        out = f"{out} host={new_host}".strip()
    if not seen_port:
        out = f"{out} port={new_port}".strip()
    return out


class TunnelManager:
    """Starts at most one tunnel for the process lifetime and reuses it."""

    def __init__(self):
        self._forwarder = None

    def ensure(self, cfg: SshTunnelConfig, remote_host: str, remote_port: int) -> tuple[str, int]:
        """Returns (local_host, local_port) to connect to instead of the real DB."""
        if self._forwarder is not None:
            return "127.0.0.1", self._forwarder.local_bind_port

        from sshtunnel import SSHTunnelForwarder  # lazy import, optional dep

        user, ssh_host, ssh_port = _parse_ssh_target(cfg.ssh_target)
        host = cfg.remote_host or remote_host
        port = cfg.remote_port or remote_port

        kwargs = dict(
            ssh_address_or_host=(ssh_host, ssh_port),
            ssh_username=user or None,
            remote_bind_address=(host, port),
            local_bind_address=("127.0.0.1", cfg.local_port or 0),
        )
        if cfg.identity_file:
            kwargs["ssh_pkey"] = cfg.identity_file

        log.info(
            "opening SSH tunnel via %s@%s:%s -> %s:%s",
            user or "<agent>", ssh_host, ssh_port, host, port,
        )
        forwarder = SSHTunnelForwarder(**kwargs)
        forwarder.start()
        self._forwarder = forwarder
        log.info("SSH tunnel up, local port %s", forwarder.local_bind_port)
        return "127.0.0.1", forwarder.local_bind_port

    def close(self) -> None:
        if self._forwarder is not None:
            self._forwarder.stop()
            self._forwarder = None


_manager = TunnelManager()


def resolve_postgres_dsn(cfg: Config) -> str:
    """Returns cfg.cloud.postgres_dsn, rewritten to go through the tunnel
    when cfg.ssh_tunnel.enabled is set."""
    dsn = cfg.cloud.postgres_dsn
    if not cfg.ssh_tunnel.enabled:
        return dsn
    remote_host, remote_port = extract_dsn_host_port(dsn, 5432)
    local_host, local_port = _manager.ensure(cfg.ssh_tunnel, remote_host, remote_port)
    return rewrite_dsn_host_port(dsn, local_host, local_port)


def shutdown() -> None:
    _manager.close()
