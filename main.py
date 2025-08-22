# main.py
import asyncio
import json
import os
import sys
import threading
import datetime
import hashlib

from PyQt6.QtWidgets import QApplication, QMessageBox

from tg_api import TelegramChatManager
from ui import TelegramWindow
from login import LoginWindow
from telethon import TelegramClient
from config import SESSIONS_FILE


def load_sessions():
    if os.path.exists(SESSIONS_FILE):
        with open(SESSIONS_FILE, "r", encoding="utf-8") as f:
            return json.load(f)
    return {}


def save_sessions(sessions):
    with open(SESSIONS_FILE, "w", encoding="utf-8") as f:
        json.dump(sessions, f, indent=4, ensure_ascii=False)


def phone_to_session(phone: str) -> str:
    h = hashlib.sha1(phone.encode()).hexdigest()[:12]
    return f"session_{h}.session"


def run_app():
    app = QApplication(sys.argv)

    # API ID/Hash
    api_id = 23435967
    api_hash = "216c60772fcaf17e0e5822e94ec86b92"

    # Event loop for Telethon in a separate thread
    loop = asyncio.new_event_loop()
    threading.Thread(target=loop.run_forever, daemon=True).start()

    sessions = load_sessions()
    client = None
    if not sessions:
        # No saved accounts → show LoginWindow
        login = LoginWindow(api_id, api_hash, loop)
        if login.exec() == LoginWindow.DialogCode.Accepted:
            client = login.client
        else:
            sys.exit(0)
    else:
        # Take the first account (can add selection later)
        phone, data = next(iter(sessions.items()))
        session_file = data["session_file"]
        client = TelegramClient(session_file, api_id, api_hash)

    # Start client
    async def start_client():
        await client.start()

    future = asyncio.run_coroutine_threadsafe(start_client(), loop)
    future.result()  # Wait for start

    # Create manager
    manager = TelegramChatManager("telegram_chat.db", client)

    # Create tables
    async def create_tables():
        await manager._create_tables()

    future = asyncio.run_coroutine_threadsafe(create_tables(), loop)
    future.result()

    # Run UI
    window = TelegramWindow(client, manager, loop)
    window.show()

    # Start background history load
    async def background_load():
        #await manager.save_chats_history(1000)
        pass

    asyncio.run_coroutine_threadsafe(background_load(), loop)

    sys.exit(app.exec())


if __name__ == "__main__":
    run_app()