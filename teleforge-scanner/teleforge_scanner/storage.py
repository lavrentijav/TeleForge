"""Cloud (and local fallback) storage for captured messages.

The Postgres schema is intentionally compatible with the desktop TeleForge
sync tables: `teleforge_sync` already exists there for the encrypted snapshot,
and this module adds plaintext, queryable event tables next to it. The desktop
client never writes these, so the two never fight over ownership.
"""

from __future__ import annotations

import abc
import logging
import sqlite3
from pathlib import Path
from typing import Iterable

log = logging.getLogger("teleforge.storage")


DELETED_COLUMNS = (
    "tenant_id", "chat_id", "message_id", "from_id", "date", "edit_date",
    "text", "media_path", "media_bytes", "mime_type", "content_hash",
    "captured_at",
)

EDITED_COLUMNS = (
    "tenant_id", "chat_id", "message_id", "from_id", "date", "edit_date",
    "revision", "text", "media_path", "content_hash", "captured_at",
)


class Storage(abc.ABC):
    @abc.abstractmethod
    def ensure_schema(self) -> None: ...

    @abc.abstractmethod
    def store_deleted(self, rows: Iterable[dict]) -> int: ...

    @abc.abstractmethod
    def store_edited(self, rows: Iterable[dict]) -> int: ...

    def close(self) -> None:  # pragma: no cover - trivial
        pass


class SqliteStorage(Storage):
    def __init__(self, path: str | Path):
        self._path = str(path)
        self._conn = sqlite3.connect(self._path)
        self._conn.execute("PRAGMA journal_mode=WAL")

    def ensure_schema(self) -> None:
        self._conn.executescript(
            """
            CREATE TABLE IF NOT EXISTS scanner_deleted (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                tenant_id TEXT, chat_id INTEGER, message_id INTEGER,
                from_id INTEGER, date INTEGER, edit_date INTEGER,
                text TEXT, media_path TEXT, media_bytes BLOB,
                mime_type TEXT, content_hash TEXT, captured_at INTEGER);
            CREATE UNIQUE INDEX IF NOT EXISTS idx_scanner_deleted_hash
                ON scanner_deleted (tenant_id, content_hash);
            CREATE TABLE IF NOT EXISTS scanner_edited (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                tenant_id TEXT, chat_id INTEGER, message_id INTEGER,
                from_id INTEGER, date INTEGER, edit_date INTEGER,
                revision INTEGER, text TEXT, media_path TEXT,
                content_hash TEXT, captured_at INTEGER);
            CREATE UNIQUE INDEX IF NOT EXISTS idx_scanner_edited_hash
                ON scanner_edited (tenant_id, content_hash);
            """
        )
        self._conn.commit()

    def _insert(self, table: str, columns, rows: Iterable[dict]) -> int:
        placeholders = ",".join("?" for _ in columns)
        sql = (
            f"INSERT OR IGNORE INTO {table} ({','.join(columns)}) "
            f"VALUES ({placeholders})"
        )
        count = 0
        for row in rows:
            self._conn.execute(sql, [row.get(c) for c in columns])
            count += 1
        self._conn.commit()
        return count

    def store_deleted(self, rows: Iterable[dict]) -> int:
        return self._insert("scanner_deleted", DELETED_COLUMNS, rows)

    def store_edited(self, rows: Iterable[dict]) -> int:
        return self._insert("scanner_edited", EDITED_COLUMNS, rows)

    def close(self) -> None:
        self._conn.close()


class PostgresStorage(Storage):
    def __init__(self, dsn: str):
        import psycopg  # imported lazily so sqlite-only installs stay slim

        self._psycopg = psycopg
        self._dsn = dsn
        log.info("connecting to PostgreSQL")
        self._conn = psycopg.connect(dsn, autocommit=False)

    def ensure_schema(self) -> None:
        with self._conn.cursor() as cur:
            cur.execute(
                """
                CREATE TABLE IF NOT EXISTS scanner_deleted (
                    id BIGSERIAL PRIMARY KEY,
                    tenant_id TEXT, chat_id BIGINT, message_id BIGINT,
                    from_id BIGINT, date BIGINT, edit_date BIGINT,
                    text TEXT, media_path TEXT, media_bytes BYTEA,
                    mime_type TEXT, content_hash TEXT, captured_at BIGINT,
                    UNIQUE (tenant_id, content_hash));
                CREATE TABLE IF NOT EXISTS scanner_edited (
                    id BIGSERIAL PRIMARY KEY,
                    tenant_id TEXT, chat_id BIGINT, message_id BIGINT,
                    from_id BIGINT, date BIGINT, edit_date BIGINT,
                    revision BIGINT, text TEXT, media_path TEXT,
                    content_hash TEXT, captured_at BIGINT,
                    UNIQUE (tenant_id, content_hash));
                """
            )
        self._conn.commit()

    def _insert(self, table: str, columns, rows: Iterable[dict]) -> int:
        placeholders = ",".join(f"%({c})s" for c in columns)
        sql = (
            f"INSERT INTO {table} ({','.join(columns)}) "
            f"VALUES ({placeholders}) "
            f"ON CONFLICT (tenant_id, content_hash) DO NOTHING"
        )
        count = 0
        with self._conn.cursor() as cur:
            for row in rows:
                cur.execute(sql, {c: row.get(c) for c in columns})
                count += 1
        self._conn.commit()
        return count

    def store_deleted(self, rows: Iterable[dict]) -> int:
        return self._insert("scanner_deleted", DELETED_COLUMNS, rows)

    def store_edited(self, rows: Iterable[dict]) -> int:
        return self._insert("scanner_edited", EDITED_COLUMNS, rows)

    def close(self) -> None:
        self._conn.close()


def make_storage(cfg) -> Storage:
    backend = cfg.cloud.backend.lower()
    if backend == "postgres":
        if not cfg.cloud.postgres_dsn.strip():
            raise ValueError("cloud.postgres_dsn is required for the postgres backend")
        from . import ssh_tunnel

        dsn = ssh_tunnel.resolve_postgres_dsn(cfg)
        log.info("using PostgreSQL storage%s", " (via SSH tunnel)" if cfg.ssh_tunnel.enabled else "")
        return PostgresStorage(dsn)
    if backend == "sqlite":
        path = Path(cfg.base_dir) / cfg.cloud.sqlite_path
        log.info("using local sqlite storage at %s", path)
        return SqliteStorage(path)
    raise ValueError(f"unknown cloud backend: {cfg.cloud.backend}")
