"""Configuration loading for the TeleForge scanner."""

from __future__ import annotations

import os
from dataclasses import dataclass, field
from pathlib import Path

try:  # Python 3.11+
    import tomllib
except ModuleNotFoundError:  # pragma: no cover - older interpreters
    import tomli as tomllib  # type: ignore


@dataclass
class TelegramConfig:
    api_id: int = 0
    api_hash: str = ""
    session_string: str = ""
    session_file: str = "teleforge_scanner"
    watch_chats: list = field(default_factory=list)
    ignore_chats: list = field(default_factory=list)


@dataclass
class CaptureConfig:
    deletions: bool = True
    edits: bool = True
    max_file_bytes: int = 5 * 1024 * 1024
    media_dir: str = "media"
    store_media_in_db: bool = False


@dataclass
class CloudConfig:
    backend: str = "postgres"
    postgres_dsn: str = ""
    tenant_id: str = ""
    sqlite_path: str = "teleforge_scanner.db"


@dataclass
class SshTunnelConfig:
    enabled: bool = False
    # "user@host" or "user@host:port" — the bastion machine ssh logs into.
    ssh_target: str = ""
    # Optional private key path; empty = ssh-agent / default ~/.ssh keys.
    identity_file: str = ""
    # DB host/port as seen *from the bastion*. Empty host = derive from
    # cloud.postgres_dsn's host=/port= tokens automatically.
    remote_host: str = ""
    remote_port: int = 0
    # 0 = pick a free local port automatically.
    local_port: int = 0
    connect_timeout: float = 15.0


@dataclass
class RuntimeConfig:
    flush_interval: float = 5.0
    batch_size: int = 200
    log_level: str = "info"


@dataclass
class Config:
    telegram: TelegramConfig = field(default_factory=TelegramConfig)
    capture: CaptureConfig = field(default_factory=CaptureConfig)
    cloud: CloudConfig = field(default_factory=CloudConfig)
    ssh_tunnel: SshTunnelConfig = field(default_factory=SshTunnelConfig)
    runtime: RuntimeConfig = field(default_factory=RuntimeConfig)
    base_dir: Path = field(default_factory=Path.cwd)


def _env_override(section: str, key: str, current):
    """Environment overrides for secrets, e.g. TFS_TELEGRAM_API_HASH."""
    name = f"TFS_{section.upper()}_{key.upper()}"
    raw = os.environ.get(name)
    if raw is None:
        return current
    if isinstance(current, bool):
        return raw.strip().lower() in ("1", "true", "yes", "on")
    if isinstance(current, int):
        return int(raw)
    if isinstance(current, float):
        return float(raw)
    return raw


def load_config(path: str | os.PathLike) -> Config:
    path = Path(path)
    data = tomllib.loads(path.read_text(encoding="utf-8"))

    cfg = Config()
    cfg.base_dir = path.resolve().parent

    for section_name, section_obj in (
        ("telegram", cfg.telegram),
        ("capture", cfg.capture),
        ("cloud", cfg.cloud),
        ("ssh_tunnel", cfg.ssh_tunnel),
        ("runtime", cfg.runtime),
    ):
        raw_section = data.get(section_name, {})
        for field_name in vars(section_obj):
            value = raw_section.get(field_name, getattr(section_obj, field_name))
            value = _env_override(section_name, field_name, value)
            setattr(section_obj, field_name, value)

    return cfg
