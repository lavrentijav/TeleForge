# login.py
import asyncio
import datetime
import json
import os

from PyQt6.QtWidgets import QDialog, QVBoxLayout, QLabel, QLineEdit, QPushButton, QMessageBox

from telethon import TelegramClient
from telethon.errors import SessionPasswordNeededError

from config import SESSIONS_FILE


class LoginWindow(QDialog):
    def __init__(self, api_id, api_hash, loop, parent=None):
        super().__init__(parent)
        self.api_id = api_id
        self.api_hash = api_hash
        self.loop = loop
        self.client = None
        self.phone = None
        self.code = None
        self.password = None

        self.setWindowTitle("Telegram Login")
        layout = QVBoxLayout(self)

        self.phone_input = QLineEdit()
        self.phone_input.setPlaceholderText("Введите номер телефона")
        layout.addWidget(QLabel("Телефон:"))
        layout.addWidget(self.phone_input)

        self.code_input = QLineEdit()
        self.code_input.setPlaceholderText("Код из Telegram")
        self.code_input.setDisabled(True)
        layout.addWidget(QLabel("Код подтверждения:"))
        layout.addWidget(self.code_input)

        self.password_input = QLineEdit()
        self.password_input.setPlaceholderText("Пароль 2FA (если есть)")
        self.password_input.setEchoMode(QLineEdit.EchoMode.Password)
        self.password_input.setDisabled(True)
        layout.addWidget(QLabel("Пароль (2FA):"))
        layout.addWidget(self.password_input)

        self.login_btn = QPushButton("Продолжить")
        self.login_btn.clicked.connect(self.start_login)
        layout.addWidget(self.login_btn)

    def start_login(self):
        from main import phone_to_session
        self.phone = self.phone_input.text().strip()
        if not self.phone:
            QMessageBox.warning(self, "Ошибка", "Введите номер телефона")
            return

        session_file = phone_to_session(self.phone)
        self.client = TelegramClient(session_file, self.api_id, self.api_hash)

        async def do_login():
            await self.client.connect()
            if not await self.client.is_user_authorized():
                await self.client.send_code_request(self.phone)
                self.code_input.setDisabled(False)
                #self.login_btn.setText("Подтвердить код")
                self.login_btn.clicked.disconnect()
                self.login_btn.clicked.connect(self.confirm_code)

        asyncio.run_coroutine_threadsafe(do_login(), self.loop).result()

    def confirm_code(self):
        code = self.code_input.text().strip()
        self.code = code

        async def do_confirm():
            try:
                await self.client.sign_in(self.phone, code)
            except SessionPasswordNeededError:
                self.password_input.setDisabled(False)
                #self.login_btn.setText("Ввести пароль")
                self.login_btn.clicked.disconnect()
                self.login_btn.clicked.connect(self.confirm_password)
                return
            except Exception as e:
                QMessageBox.critical(self, "Ошибка", str(e))
                return

            # Success
            me = await self.client.get_me()
            sessions = {}
            if os.path.exists(SESSIONS_FILE):
                with open(SESSIONS_FILE, "r", encoding="utf-8") as f:
                    sessions = json.load(f)
            sessions[self.phone] = {
                "session_file": self.client.session.filename,
                "created_at": datetime.datetime.now().isoformat(),
            }
            with open(SESSIONS_FILE, "w", encoding="utf-8") as f:
                json.dump(sessions, f, indent=4, ensure_ascii=False)
            self.accept()

        asyncio.run_coroutine_threadsafe(do_confirm(), self.loop).result()

    def confirm_password(self):
        password = self.password_input.text().strip()
        self.password = password

        async def do_pass():
            try:
                await self.client.sign_in(password=self.password)
                me = await self.client.get_me()
                sessions = {}
                if os.path.exists(SESSIONS_FILE):
                    with open(SESSIONS_FILE, "r", encoding="utf-8") as f:
                        sessions = json.load(f)
                sessions[self.phone] = {
                    "session_file": self.client.session.filename,
                    "created_at": datetime.datetime.now().isoformat(),
                }
                with open(SESSIONS_FILE, "w", encoding="utf-8") as f:
                    json.dump(sessions, f, indent=4, ensure_ascii=False)
                self.close()
            except Exception as e:
                QMessageBox.critical(self, "Ошибка", str(e))

        asyncio.run_coroutine_threadsafe(do_pass(), self.loop).result()