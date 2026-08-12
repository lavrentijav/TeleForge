"""Core scanner: watches chats via Telethon, captures deletions/edits/files."""

from __future__ import annotations

import asyncio
import hashlib
import logging
import time
from pathlib import Path

from telethon import TelegramClient, events
from telethon.sessions import StringSession

from .config import Config
from .storage import Storage, make_storage

log = logging.getLogger("teleforge.scanner")


def _content_hash(*parts) -> str:
    h = hashlib.sha256()
    for part in parts:
        h.update(str(part).encode("utf-8", "replace"))
        h.update(b"\x1f")
    return h.hexdigest()


class Scanner:
    def __init__(self, cfg: Config):
        self.cfg = cfg
        self.storage: Storage = make_storage(cfg)
        self._client = self._make_client(cfg)
        self._tenant = cfg.cloud.tenant_id or ""
        self._media_dir = Path(cfg.base_dir) / cfg.capture.media_dir
        self._media_dir.mkdir(parents=True, exist_ok=True)

        self._deleted_queue: list[dict] = []
        self._edited_queue: list[dict] = []
        # Recent text cache so a delete event (which carries no content) can be
        # resolved back to the message body Telethon last saw.
        self._recent: dict[tuple[int, int], dict] = {}
        self._watch = set(str(x) for x in cfg.telegram.watch_chats)
        self._ignore = set(str(x) for x in cfg.telegram.ignore_chats)

    @staticmethod
    def _make_client(cfg: Config) -> TelegramClient:
        if cfg.telegram.session_string:
            session = StringSession(cfg.telegram.session_string)
        else:
            session = str(Path(cfg.base_dir) / cfg.telegram.session_file)
        return TelegramClient(session, cfg.telegram.api_id, cfg.telegram.api_hash)

    def _chat_watched(self, chat_id) -> bool:
        cid = str(chat_id)
        if cid in self._ignore:
            return False
        if not self._watch:
            return True
        return cid in self._watch

    async def _resolve_tenant(self) -> None:
        if self._tenant:
            return
        me = await self._client.get_me()
        # Same shape as the desktop PgTenantId: sha256(id:phone).
        material = f"{me.id}:{me.phone or ''}".encode("utf-8")
        self._tenant = hashlib.sha256(material).hexdigest()
        log.info("derived tenant id %s", self._tenant[:12])

    async def _maybe_download(self, message) -> tuple[str | None, bytes | None, str | None]:
        limit = self.cfg.capture.max_file_bytes
        if limit <= 0 or not message.file:
            return None, None, None
        size = message.file.size or 0
        if size <= 0 or size > limit:
            return None, None, message.file.mime_type
        try:
            data = await message.download_media(bytes)
        except Exception as exc:  # network / permission
            log.warning("download failed for %s: %s", message.id, exc)
            return None, None, message.file.mime_type
        name = f"{message.chat_id}_{message.id}_{int(time.time())}"
        suffix = message.file.ext or ""
        path = self._media_dir / f"{name}{suffix}"
        try:
            path.write_bytes(data)
        except OSError as exc:
            log.warning("could not persist media %s: %s", path, exc)
            path = None
        db_bytes = data if self.cfg.capture.store_media_in_db else None
        return (str(path) if path else None), db_bytes, message.file.mime_type

    def _remember(self, message) -> None:
        media_path = None
        text = message.message or ""
        self._recent[(message.chat_id, message.id)] = {
            "from_id": getattr(message.from_id, "user_id", None)
            or getattr(message, "sender_id", None),
            "date": int(message.date.timestamp()) if message.date else 0,
            "text": text,
            "media_path": media_path,
            "mime_type": message.file.mime_type if message.file else None,
        }
        # Bounded cache.
        if len(self._recent) > 20000:
            for key in list(self._recent.keys())[:5000]:
                self._recent.pop(key, None)

    def _register_handlers(self) -> None:
        client = self._client

        @client.on(events.NewMessage())
        async def on_new(event):
            if not self._chat_watched(event.chat_id):
                return
            message = event.message
            path, db_bytes, mime = await self._maybe_download(message)
            entry = {
                "from_id": getattr(message, "sender_id", None),
                "date": int(message.date.timestamp()) if message.date else 0,
                "text": message.message or "",
                "media_path": path,
                "media_bytes": db_bytes,
                "mime_type": mime,
            }
            self._recent[(event.chat_id, message.id)] = entry

        if self.cfg.capture.edits:
            @client.on(events.MessageEdited())
            async def on_edit(event):
                if not self._chat_watched(event.chat_id):
                    return
                message = event.message
                text = message.message or ""
                edit_date = int(message.edit_date.timestamp()) if message.edit_date else 0
                row = {
                    "tenant_id": self._tenant,
                    "chat_id": event.chat_id,
                    "message_id": message.id,
                    "from_id": getattr(message, "sender_id", None),
                    "date": int(message.date.timestamp()) if message.date else 0,
                    "edit_date": edit_date,
                    "revision": edit_date,
                    "text": text,
                    "media_path": None,
                    "content_hash": _content_hash(
                        event.chat_id, message.id, edit_date, text),
                    "captured_at": int(time.time()),
                }
                self._edited_queue.append(row)
                self._recent[(event.chat_id, message.id)] = {
                    **self._recent.get((event.chat_id, message.id), {}),
                    "text": text,
                }

        if self.cfg.capture.deletions:
            @client.on(events.MessageDeleted())
            async def on_delete(event):
                if event.chat_id is not None and not self._chat_watched(event.chat_id):
                    return
                for message_id in event.deleted_ids:
                    key = (event.chat_id, message_id)
                    cached = self._recent.pop(key, {})
                    text = cached.get("text", "")
                    row = {
                        "tenant_id": self._tenant,
                        "chat_id": event.chat_id or 0,
                        "message_id": message_id,
                        "from_id": cached.get("from_id"),
                        "date": cached.get("date", 0),
                        "edit_date": 0,
                        "text": text,
                        "media_path": cached.get("media_path"),
                        "media_bytes": cached.get("media_bytes"),
                        "mime_type": cached.get("mime_type"),
                        "content_hash": _content_hash(
                            event.chat_id, message_id, text),
                        "captured_at": int(time.time()),
                    }
                    self._deleted_queue.append(row)

    async def _flush_loop(self) -> None:
        interval = max(1.0, float(self.cfg.runtime.flush_interval))
        batch = max(1, int(self.cfg.runtime.batch_size))
        while True:
            await asyncio.sleep(interval)
            try:
                if self._deleted_queue:
                    chunk = self._deleted_queue[:batch]
                    del self._deleted_queue[:batch]
                    n = self.storage.store_deleted(chunk)
                    log.info("stored %d deleted", n)
                if self._edited_queue:
                    chunk = self._edited_queue[:batch]
                    del self._edited_queue[:batch]
                    n = self.storage.store_edited(chunk)
                    log.info("stored %d edited", n)
            except Exception as exc:  # keep the loop alive on DB hiccups
                log.error("flush failed, will retry: %s", exc)

    async def run(self) -> None:
        self.storage.ensure_schema()
        await self._client.start()
        await self._resolve_tenant()
        self._register_handlers()
        log.info("scanner running (tenant=%s)", self._tenant[:12])
        flusher = asyncio.create_task(self._flush_loop())
        try:
            await self._client.run_until_disconnected()
        finally:
            flusher.cancel()
            self.storage.close()
