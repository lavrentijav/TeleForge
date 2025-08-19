# tg_api.py
import asyncio
import datetime
import hashlib
import logging
import os
import time
import threading

import aiosqlite
from telethon import TelegramClient, events, types
from telethon.errors import RPCError
from telethon.tl.types import PeerUser, PeerChat, PeerChannel

logging.basicConfig(level=logging.INFO, format="%(asctime)s - %(levelname)s - %(message)s")


class TelegramChatManager:
    def __init__(self, db_path: str, client: TelegramClient):
        self.db_path = db_path
        self.client = client
        self.assets_path = "assets/"
        self.processed_events = set()
        self.max_cache_size = 1000
        self.db_semaphore = asyncio.Semaphore(1)
        self.api_semaphore = asyncio.Semaphore(1)
        self._register_handlers()

    async def _execute_with_semaphore(self, query: str, params=()) -> bool:
        start_time = time.time()
        async with self.db_semaphore:
            async with aiosqlite.connect(self.db_path, timeout=10) as conn:
                try:
                    await conn.execute(query, params)
                    await conn.commit()
                    logging.debug(f"DB_EXEC: query={query[:50]}..., time={time.time() - start_time:.2f}s")
                    return True
                except Exception as exc:
                    logging.exception(
                        f"DB_EXEC_ERR: query={query[:50]}..., time={time.time() - start_time:.2f}s, exc={exc}"
                    )
                    return False

    async def _fetch_with_semaphore(self, query: str, params=()):
        start_time = time.time()
        async with self.db_semaphore:
            async with aiosqlite.connect(self.db_path, timeout=10) as conn:
                async with conn.execute(query, params) as cursor:
                    result = await cursor.fetchall()
                    logging.debug(f"DB_FETCH: query={query[:50]}..., time={time.time() - start_time:.2f}s")
                    return result

    async def _create_tables(self):
        async with self.db_semaphore:
            async with aiosqlite.connect(self.db_path, timeout=10) as conn:
                await conn.executescript("""
                    CREATE TABLE IF NOT EXISTS users (
                        user_id INTEGER PRIMARY KEY,
                        username TEXT,
                        first_name TEXT,
                        last_name TEXT
                    );

                    CREATE TABLE IF NOT EXISTS chats (
                        chat_id INTEGER PRIMARY KEY,
                        chat_type TEXT NOT NULL,
                        title TEXT,
                        description TEXT,
                        rules TEXT
                    );

                    CREATE TABLE IF NOT EXISTS messages (
                        message_id INTEGER NOT NULL,
                        chat_id INTEGER NOT NULL,
                        sender_id INTEGER NOT NULL,
                        content TEXT,
                        created_at INTEGER NOT NULL,
                        reply_to INTEGER,
                        forwarded_from INTEGER,
                        message_type TEXT,
                        media_path TEXT,
                        version INTEGER DEFAULT 1,
                        pinned INTEGER DEFAULT 0,
                        history_id TEXT NOT NULL PRIMARY KEY,
                        read_status INTEGER DEFAULT 0,
                        deleted INTEGER DEFAULT 0,
                        edited INTEGER DEFAULT 0,
                        FOREIGN KEY(chat_id) REFERENCES chats(chat_id),
                        FOREIGN KEY(sender_id) REFERENCES users(user_id),
                        FOREIGN KEY(reply_to) REFERENCES messages(message_id),
                        FOREIGN KEY(forwarded_from) REFERENCES messages(message_id),
                        UNIQUE(chat_id, message_id)
                    );

                    CREATE INDEX IF NOT EXISTS idx_messages_chat_created ON messages (chat_id, created_at);
                    CREATE INDEX IF NOT EXISTS idx_messages_chat_sender_id ON messages (chat_id, sender_id, message_id);

                    CREATE TABLE IF NOT EXISTS message_events (
                        event_id INTEGER PRIMARY KEY,
                        history_id TEXT NOT NULL,
                        event_type TEXT NOT NULL,
                        content TEXT,
                        created_at INTEGER NOT NULL,
                        reply_to INTEGER,
                        forwarded_from INTEGER,
                        message_type TEXT,
                        media_path TEXT,
                        replaced_content TEXT,
                        version INTEGER NOT NULL,
                        pinned INTEGER DEFAULT 0,
                        FOREIGN KEY(reply_to) REFERENCES messages(message_id),
                        FOREIGN KEY(forwarded_from) REFERENCES messages(message_id)
                    );

                    CREATE INDEX IF NOT EXISTS idx_message_events_history_id ON message_events (history_id);

                    CREATE TABLE IF NOT EXISTS attachments (
                        attachment_id INTEGER PRIMARY KEY,
                        message_id INTEGER NOT NULL,
                        attachment_type TEXT NOT NULL,
                        file_path TEXT,
                        created_at INTEGER NOT NULL,
                        FOREIGN KEY(message_id) REFERENCES messages(message_id)
                    );
                """)
                await conn.commit()
        logging.info("DB_INIT: tables created/checked")

    def _generate_history_id(self, chat_id: int, message_id: int) -> str:
        return hashlib.sha224(f"{chat_id}{message_id}".encode()).hexdigest()[:32]

    async def check_user_exists(self, user_id: int) -> bool:
        result = await self._fetch_with_semaphore("SELECT 1 FROM users WHERE user_id = ?", (user_id,))
        return bool(result)

    async def check_chat_exists(self, chat_id: int) -> bool:
        result = await self._fetch_with_semaphore("SELECT 1 FROM chats WHERE chat_id = ?", (chat_id,))
        return bool(result)

    async def check_message_exists(self, chat_id: int, message_id: int) -> bool:
        result = await self._fetch_with_semaphore(
            "SELECT 1 FROM messages WHERE chat_id = ? AND message_id = ?", (chat_id, message_id)
        )
        return bool(result)

    async def save_user(self, user_id: int):
        if not await self.check_user_exists(user_id):
            try:
                entity = await self.client.get_entity(user_id)
                username = entity.username or None
                if isinstance(entity, types.User):
                    first_name = entity.first_name or None
                    last_name = entity.last_name or None
                else:
                    first_name = entity.title
                    last_name = None
                await self._execute_with_semaphore(
                    "INSERT INTO users (user_id, username, first_name, last_name) VALUES (?, ?, ?, ?)",
                    (user_id, username, first_name, last_name),
                )
                logging.info(f"USR_SAVED: id={user_id}, uname={username}")
            except (RPCError, ValueError) as exc:
                logging.error(f"ERR_GET_ENTITY: id={user_id}, exc={exc}")

    async def save_chat(
        self, chat_id: int, chat_type: str, title: str, description: str = None, rules: str = None
    ) -> int:
        if not await self.check_chat_exists(chat_id):
            await self._execute_with_semaphore(
                "INSERT INTO chats (chat_id, chat_type, title, description, rules) VALUES (?, ?, ?, ?, ?)",
                (chat_id, chat_type, title, description, rules),
            )
            logging.info(f"CHAT_SAVED: id={chat_id}")
        return chat_id

    async def save_message(
        self,
        chat_id: int,
        sender_id: int,
        message_id: int,
        content: str,
        created_at: int,
        reply_to: int = None,
        forwarded_from: int = None,
        message_type: str = None,
        media_path: str = None,
        pinned: bool = False,
        ignore_existing: bool = False,
    ):
        if ignore_existing and await self.check_message_exists(chat_id, message_id):
            return None

        try:
            chat = await self.client.get_entity(chat_id)
            chat_type = "channel" if isinstance(chat, (types.Chat, types.Channel)) else "private"
            title = getattr(chat, "title", None) or getattr(chat, "username", None)
            await self.save_chat(chat_id, chat_type, title)
        except RPCError as exc:
            logging.error(f"ERR_GET_CHAT: id={chat_id}, exc={exc}")
            return None

        effective_sender_id = sender_id if sender_id != 0 else chat_id
        if sender_id != 0:
            await self.save_user(effective_sender_id)
        else:
            await self.save_chat(effective_sender_id, "channel", title=None)
            sender_id = effective_sender_id

        prev_message_id = message_id - 1
        existing = await self._fetch_with_semaphore(
            """
            SELECT history_id, version, created_at, content
            FROM messages
            WHERE chat_id = ? AND sender_id = ? AND message_id = ? AND deleted = 0
            """,
            (chat_id, sender_id, prev_message_id),
        )
        if existing:
            history_id, version, prev_created_at, prev_content = existing[0]
            if content == prev_content and abs(created_at - prev_created_at) < 15:
                logging.info(f"DUP_DETECTED: chat_id={chat_id}, new_id={message_id}, prev_id={prev_message_id}")
                new_version = version + 1
                await self._execute_with_semaphore(
                    """
                    UPDATE messages
                    SET message_id = ?, created_at = ?, reply_to = ?, forwarded_from = ?, message_type = ?,
                        media_path = ?, version = ?, pinned = ?, edited = 1
                    WHERE message_id = ? AND chat_id = ?
                    """,
                    (
                        message_id,
                        created_at,
                        reply_to,
                        forwarded_from,
                        message_type,
                        media_path,
                        new_version,
                        pinned,
                        prev_message_id,
                        chat_id,
                    ),
                )
                await self._execute_with_semaphore(
                    """
                    INSERT INTO message_events (history_id, event_type, content, created_at, reply_to, forwarded_from,
                                                message_type, media_path, replaced_content, version, pinned)
                    VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                    """,
                    (
                        history_id,
                        "edited",
                        content,
                        created_at,
                        reply_to,
                        forwarded_from,
                        message_type,
                        media_path,
                        prev_content,
                        new_version,
                        pinned,
                    ),
                )
                logging.info(f"MSG_UPDATED_DUP: chat_id={chat_id}, msg_id={message_id}")
                return await self._fetch_with_semaphore(
                    "SELECT chat_id, sender_id, version, content FROM messages WHERE message_id = ? AND chat_id = ?",
                    (message_id, chat_id),
                )

        history_id = self._generate_history_id(chat_id, message_id)
        try:
            await self._execute_with_semaphore(
                """
                INSERT INTO messages (message_id, chat_id, sender_id, content, created_at, reply_to, forwarded_from,
                                      message_type, media_path, version, pinned, history_id, read_status, deleted, edited)
                VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, 1, ?, ?, 0, 0, 0)
                """,
                (
                    message_id,
                    chat_id,
                    sender_id,
                    content,
                    created_at,
                    reply_to,
                    forwarded_from,
                    message_type,
                    media_path,
                    pinned,
                    history_id,
                ),
            )
            await self._execute_with_semaphore(
                """
                INSERT INTO message_events (history_id, event_type, content, created_at, reply_to, forwarded_from,
                                            message_type, media_path, replaced_content, version, pinned)
                VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                """,
                (
                    history_id,
                    "created",
                    content,
                    created_at,
                    reply_to,
                    forwarded_from,
                    message_type,
                    media_path,
                    content,
                    1,
                    pinned,
                ),
            )
            logging.info(f"MSG_SAVED: chat_id={chat_id}, msg_id={message_id}")
        except aiosqlite.IntegrityError:
            logging.warning(f"INT_ERR_SAVE_MSG: chat_id={chat_id}, msg_id={message_id}")
            return await self.update_message(
                message_id,
                chat_id,
                content,
                created_at,
                sender_id,
                reply_to,
                forwarded_from,
                message_type,
                media_path,
                pinned,
            )
        except Exception as exc:
            logging.exception(f"UNEXP_ERR_SAVE_MSG: chat_id={chat_id}, msg_id={message_id}, exc={exc}")
            return None

        return await self._fetch_with_semaphore(
            "SELECT chat_id, sender_id, version, content FROM messages WHERE message_id = ? AND chat_id = ?",
            (message_id, chat_id),
        )

    async def update_message(
        self,
        message_id: int,
        chat_id: int,
        content: str,
        created_at: int,
        sender_id: int,
        reply_to: int = None,
        forwarded_from: int = None,
        message_type: str = None,
        media_path: str = None,
        pinned: bool = False,
    ):
        result = await self._fetch_with_semaphore(
            "SELECT chat_id, sender_id, version, content FROM messages WHERE message_id = ? AND chat_id = ?",
            (message_id, chat_id),
        )
        if not result:
            logging.warning(f"MSG_NOT_FOUND_UPD: chat_id={chat_id}, msg_id={message_id}")
            return await self.save_message(
                chat_id,
                sender_id,
                message_id,
                content,
                created_at,
                reply_to,
                forwarded_from,
                message_type,
                media_path,
                pinned,
            )

        _, stored_sender_id, current_version, old_content = result[0]
        new_version = current_version + 1
        history_id = self._generate_history_id(chat_id, message_id)
        sender_id = sender_id or stored_sender_id

        if sender_id and not await self.check_user_exists(sender_id):
            await self.save_user(sender_id)

        try:
            await self._execute_with_semaphore(
                """
                UPDATE messages
                SET content = ?, reply_to = ?, forwarded_from = ?, message_type = ?,
                    media_path = ?, version = ?, pinned = ?, edited = 1
                WHERE message_id = ? AND chat_id = ?
                """,
                (
                    content,
                    reply_to,
                    forwarded_from,
                    message_type,
                    media_path,
                    new_version,
                    pinned,
                    message_id,
                    chat_id,
                ),
            )
            await self._execute_with_semaphore(
                """
                INSERT INTO message_events (history_id, event_type, content, created_at, reply_to, forwarded_from,
                                            message_type, media_path, replaced_content, version, pinned)
                VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                """,
                (
                    history_id,
                    "edited",
                    content,
                    created_at,
                    reply_to,
                    forwarded_from,
                    message_type,
                    media_path,
                    old_content,
                    new_version,
                    pinned,
                ),
            )
            logging.info(f"MSG_UPDATED: chat_id={chat_id}, msg_id={message_id}")
            return message_id
        except Exception as exc:
            logging.exception(f"ERR_UPD_MSG: chat_id={chat_id}, msg_id={message_id}, exc={exc}")
            return None

    async def delete_message(self, chat_id: int, message_id: int, created_at: int):
        result = await self._fetch_with_semaphore(
            """
            SELECT content, created_at, reply_to, forwarded_from, message_type, media_path, version, pinned
            FROM messages WHERE message_id = ? AND chat_id = ?
            """,
            (message_id, chat_id),
        )
        if result:
            (
                content,
                old_created_at,
                reply_to,
                forwarded_from,
                message_type,
                media_path,
                version,
                pinned,
            ) = result[0]
            history_id = self._generate_history_id(chat_id, message_id)
            try:
                await self._execute_with_semaphore(
                    "UPDATE messages SET deleted = 1 WHERE message_id = ? AND chat_id = ?",
                    (message_id, chat_id),
                )
                await self._execute_with_semaphore(
                    """
                    INSERT INTO message_events (history_id, event_type, content, created_at, reply_to, forwarded_from,
                                                message_type, media_path, replaced_content, version, pinned)
                    VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                    """,
                    (
                        history_id,
                        "deleted",
                        content,
                        created_at,
                        reply_to,
                        forwarded_from,
                        message_type,
                        media_path,
                        content,
                        version,
                        pinned,
                    ),
                )
                logging.info(f"MSG_DELETED: chat_id={chat_id}, msg_id={message_id}")
            except Exception as exc:
                logging.exception(f"ERR_DEL_MSG: chat_id={chat_id}, msg_id={message_id}, exc={exc}")

    async def save_attachment(
        self, message_id: int, attachment_type: str, created_at: int, file_path: str = None
    ) -> bool:
        try:
            await self._execute_with_semaphore(
                "INSERT INTO attachments (message_id, attachment_type, file_path, created_at) VALUES (?, ?, ?, ?)",
                (message_id, attachment_type, file_path, created_at),
            )
            logging.info(f"ATT_SAVED: msg_id={message_id}, type={attachment_type}")
            return True
        except Exception as exc:
            logging.exception(f"ERR_SAVE_ATT: msg_id={message_id}, exc={exc}")
            return False

    async def get_last_messages(self, chat_id: int, limit: int = 100):
        if not await self.check_chat_exists(chat_id):
            raise ValueError("Chat does not exist")
        return await self._fetch_with_semaphore(
            "SELECT * FROM messages WHERE chat_id = ? AND deleted = 0 ORDER BY created_at DESC LIMIT ?",
            (chat_id, limit),
        )

    async def get_message_history(self, chat_id: int, message_id: int):
        history_id = self._generate_history_id(chat_id, message_id)
        return await self._fetch_with_semaphore(
            "SELECT * FROM message_events WHERE history_id = ? ORDER BY created_at ASC", (history_id,)
        )

    async def get_all_chats(self):
        return await self._fetch_with_semaphore("SELECT * FROM chats")

    async def get_messages_for_batch(self, chat_id, direction="older", min_id=None, max_id=None, limit=50):
        if direction == "older":
            if min_id is None:
                query = """
                SELECT m.message_id, m.content, u.username, m.created_at, m.sender_id
                FROM messages m JOIN users u ON m.sender_id = u.user_id
                WHERE m.chat_id = ?
                ORDER BY m.message_id DESC LIMIT ?
                """
                params = (chat_id, limit)
            else:
                query = """
                SELECT m.message_id, m.content, u.username, m.created_at, m.sender_id
                FROM messages m JOIN users u ON m.sender_id = u.user_id
                WHERE m.chat_id = ? AND m.message_id > ?
                ORDER BY m.message_id DESC LIMIT ?
                """
                params = (chat_id, min_id, limit)
        elif direction == "newer":
            if max_id is None:
                return []
            query = """
            SELECT m.message_id, m.content, u.username, m.created_at, m.sender_id
            FROM messages m JOIN users u ON m.sender_id = u.user_id
            WHERE m.chat_id = ? AND m.message_id < ?
            ORDER BY m.message_id DESC LIMIT ?
            """
            params = (chat_id, max_id, limit)
        return await self._fetch_with_semaphore(query, params)

    async def get_message_content(self, chat_id, message_id):
        result = await self._fetch_with_semaphore(
            "SELECT content FROM messages WHERE message_id = ? AND chat_id = ?",
            (message_id, chat_id),
        )
        return result[0][0] if result else None

    async def save_chats_history(self, limit: int = 100):
        try:
            start_time = time.time()
            async for dialog in self.client.iter_dialogs():
                chat_id = dialog.id
                logging.info(f"LOADING_CHAT: id={chat_id}")
                try:
                    time.sleep(1/10 - (start_time - time.time()))
                    async with self.api_semaphore:
                        chat = await self.client.get_entity(chat_id)
                    chat_type = "channel" if isinstance(chat, (types.Chat, types.Channel)) else "private"
                    title = getattr(chat, "title", None) or getattr(chat, "username", None)
                    await self.save_chat(chat_id, chat_type, title)
                except RPCError as exc:
                    logging.error(f"ERR_LOAD_CHAT: id={chat_id}, exc={exc}")
                    continue
                start_time = time.time()


                await self.save_chat_history(chat_id, limit)


            logging.info("HIST_LOADED")
        except Exception as exc:
            logging.exception(f"UNEXP_ERR_LOAD_HIST: exc={exc}")

    async def save_chat_history(self, chat_id, limit: int = 100):
        messages_to_save = []
        async with self.api_semaphore:
            messages = self.client.iter_messages(chat_id, limit=limit)
        async for message in messages:
            sender = message.from_id or message.peer_id
            if isinstance(sender, PeerUser):
                sender_id = sender.user_id
            elif isinstance(sender, PeerChat):
                sender_id = sender.chat_id
            elif isinstance(sender, PeerChannel):
                sender_id = sender.channel_id
            else:
                continue

            message_id = message.id
            content = message.message
            created_at = int(message.date.timestamp())
            reply_to = message.reply_to_msg_id
            forwarded_from = None
            message_type = "text" if content else "media" if message.media else None
            pinned = message.pinned
            media_path = None

            if message.media:
                if isinstance(message.media, types.MessageMediaPhoto):
                    media_path = f"{self.assets_path}photo_{message_id}.jpg"
                elif isinstance(message.media, types.MessageMediaDocument):
                    media_path = f"{self.assets_path}document_{message_id}"

            history_id = self._generate_history_id(chat_id, message_id)
            messages_to_save.append(
                (
                    message_id,
                    chat_id,
                    sender_id,
                    content,
                    created_at,
                    reply_to,
                    forwarded_from,
                    message_type,
                    media_path,
                    pinned,
                    history_id,
                )
            )

        if messages_to_save:
            async with self.db_semaphore:
                async with aiosqlite.connect(self.db_path, timeout=10) as conn:
                    try:
                        await conn.executemany(
                            """
                            INSERT OR IGNORE INTO messages (message_id, chat_id, sender_id, content, created_at,
                                                            reply_to, forwarded_from,
                                                            message_type, media_path, version, pinned, history_id,
                                                            read_status, deleted, edited)
                            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, 1, ?, ?, 0, 0, 0)
                            """,
                            messages_to_save,
                        )
                        await conn.executemany(
                            """
                            INSERT OR IGNORE INTO message_events (history_id, event_type, content, created_at, reply_to,
                                                                  forwarded_from,
                                                                  message_type, media_path, replaced_content, version,
                                                                  pinned)
                            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
                            """,
                            [
                                (m[10], "created", m[3], m[4], m[5], m[6], m[7], m[8], m[3], 1, m[9])
                                for m in messages_to_save
                            ],
                        )
                        await conn.commit()
                        logging.info(f"HIST_LOADED: chat_id={chat_id}, msgs={len(messages_to_save)}")
                    except Exception as exc:
                        logging.error(f"ERR_LOAD_MSGS: chat_id={chat_id}, exc={exc}")

    def _register_handlers(self):
        @self.client.on(events.NewMessage())
        async def handler_new_message(event):
            event_key = (event.chat_id, event.message.id)
            if event_key in self.processed_events:
                logging.info(f"EVT_DUP_IGN_NEW: key={event_key}")
                return
            self.processed_events.add(event_key)
            if len(self.processed_events) > self.max_cache_size:
                self.processed_events = set(list(self.processed_events)[-self.max_cache_size // 2 :])
                logging.info("CACHE_TRIMMED")
            message = event.message
            chat_id = event.chat_id
            sender = message.from_id
            sender_id = 0
            if isinstance(sender, PeerUser):
                sender_id = sender.user_id
            elif isinstance(sender, PeerChat):
                sender_id = sender.chat_id
            elif isinstance(sender, PeerChannel):
                sender_id = sender.channel_id

            message_id = message.id
            content = message.message
            created_at = int(message.date.timestamp())
            reply_to = message.reply_to_msg_id
            forwarded_from = None
            message_type = "text" if content else "media" if message.media else None
            pinned = message.pinned
            media_path = None

            if message.media:
                if isinstance(message.media, types.MessageMediaPhoto):
                    media_path = f"{self.assets_path}photo_{message_id}.jpg"
                elif isinstance(message.media, types.MessageMediaDocument):
                    media_path = f"{self.assets_path}document_{message_id}"

            await self.save_message(
                chat_id,
                sender_id,
                message_id,
                content,
                created_at,
                reply_to,
                forwarded_from,
                message_type,
                media_path,
                pinned,
            )
            self.processed_events.discard(event_key)

        @self.client.on(events.MessageEdited())
        async def handler_edit_message(event):
            event_key = (event.chat_id, event.message.id)
            if event_key in self.processed_events:
                logging.info(f"EVT_DUP_IGN_EDIT: key={event_key}")
                return
            self.processed_events.add(event_key)
            if len(self.processed_events) > self.max_cache_size:
                self.processed_events = set(list(self.processed_events)[-self.max_cache_size // 2 :])
                logging.info("CACHE_TRIMMED")
            message = event.message
            chat_id = event.chat_id
            sender = message.from_id
            sender_id = 0
            if isinstance(sender, PeerUser):
                sender_id = sender.user_id
            elif isinstance(sender, PeerChat):
                sender_id = sender.chat_id
            elif isinstance(sender, PeerChannel):
                sender_id = sender.channel_id

            message_id = message.id
            content = message.message
            created_at = int(message.edit_date.timestamp()) if message.edit_date else int(message.date.timestamp())
            reply_to = message.reply_to_msg_id
            forwarded_from = None
            message_type = "text" if content else "media" if message.media else None
            pinned = message.pinned
            media_path = None

            if message.media:
                if isinstance(message.media, types.MessageMediaPhoto):
                    media_path = f"{self.assets_path}photo_{message_id}_edited.jpg"
                elif isinstance(message.media, types.MessageMediaDocument):
                    media_path = f"{self.assets_path}document_{message_id}_edited"

            await self.update_message(
                message_id,
                chat_id,
                content,
                created_at,
                sender_id,
                reply_to,
                forwarded_from,
                message_type,
                media_path,
                pinned,
            )
            self.processed_events.discard(event_key)

        @self.client.on(events.MessageDeleted())
        async def handler_delete_message(event):
            created_at = int(datetime.datetime.now().timestamp())
            for message_id in event.deleted_ids:
                chat_id = event.chat_id
                await self.delete_message(chat_id, message_id, created_at)
                logging.info(f"EVT_DEL: msg_id={message_id}")

    async def close(self):
        self.processed_events.clear()
        logging.info("MGR_CLOSED")
