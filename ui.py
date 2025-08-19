# ui.py
import asyncio
import datetime
import time

from PyQt6.QtCore import Qt, QTimer
from PyQt6.QtGui import QFontMetrics, QIcon, QAction
from PyQt6.QtWidgets import (
    QMainWindow,
    QWidget,
    QVBoxLayout,
    QHBoxLayout,
    QListWidget,
    QListWidgetItem,
    QPlainTextEdit,
    QPushButton,
    QLabel,
    QSizePolicy,
    QScrollArea,
    QMenu,
    QApplication, QLineEdit,
)

from telethon import TelegramClient
from tg_api import TelegramChatManager


class MessageGroupWidget(QWidget):
    def __init__(self, username: str, messages: list, is_own: bool = False, parent=None):
        super().__init__(parent=parent)

        # messages: [(content, timestamp, message_id, is_me), ...]
        self.message_ids = [msg[2] for msg in messages]
        self.parent_window = parent  # For access to show_context_menu

        self.main_layout = QHBoxLayout(self)
        self.main_layout.setContentsMargins(10, 5, 10, 5)
        self.main_layout.setSpacing(0)
        self.messages_widgets = []

        bubble_container = QWidget()
        bubble_layout = QVBoxLayout(bubble_container)
        bubble_layout.setContentsMargins(0, 0, 0, 0)
        bubble_layout.setSpacing(2)

        large_radius = 12
        small_radius = 4

        for i, (content, timestamp, msg_id) in enumerate(messages):
            sub_bubble = QWidget()
            sub_bubble_layout = QVBoxLayout(sub_bubble)
            sub_bubble_layout.setContentsMargins(10, 4, 10, 4)
            sub_bubble_layout.setSpacing(2)

            top_radius = large_radius if i == 0 else small_radius
            bottom_radius = large_radius if i == len(messages) - 1 else small_radius

            if is_own:
                sub_bubble.setStyleSheet(
                    f"""
                    background-color: #3390FF;
                    border-top-left-radius: {top_radius}px;
                    border-top-right-radius: {small_radius}px;
                    border-bottom-left-radius: {bottom_radius}px;
                    border-bottom-right-radius: {small_radius}px;
                    color: white;
                """
                )
            else:
                sub_bubble.setStyleSheet(
                    f"""
                    background-color: #2A2F3B;
                    border-top-left-radius: {small_radius}px;
                    border-top-right-radius: {top_radius}px;
                    border-bottom-left-radius: {small_radius}px;
                    border-bottom-right-radius: {bottom_radius}px;
                    color: white;
                """
                )

            if i == 0:
                username_label = QLabel(username)
                username_label.setStyleSheet(
                    "font-weight: bold; color: green;" if is_own else "font-weight: bold; color: #2c7be5;"
                )
                sub_bubble_layout.addWidget(username_label)

            message_label = QLabel(content)
            message_label.setWordWrap(True)
            message_label.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Minimum)
            self.messages_widgets.append(message_label)
            fm = QFontMetrics(message_label.font())
            text_height = fm.boundingRect(0, 0, 400, 0, Qt.TextFlag.TextWordWrap, content).height()
            line_height = fm.lineSpacing()
            message_label.setMinimumHeight(text_height + line_height)

            timestamp_label = QLabel(timestamp)
            timestamp_label.setStyleSheet("color: gray; font-size: 10px;")
            timestamp_label.setAlignment(Qt.AlignmentFlag.AlignRight)

            sub_bubble_layout.addWidget(message_label)
            sub_bubble_layout.addWidget(timestamp_label)

            # Custom context menu for sub_bubble
            sub_bubble.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
            sub_bubble.customContextMenuRequested.connect(lambda pos, mid=msg_id: self.show_context_menu(pos, mid))

            bubble_layout.addWidget(sub_bubble)

        if is_own:
            self.main_layout.addStretch()
            self.main_layout.addWidget(bubble_container)
        else:
            self.main_layout.addWidget(bubble_container)
            self.main_layout.addStretch()

        self.setLayout(self.main_layout)

    def resizeEvent(self, event):
        max_width_percent = 0.7
        new_max_width = int(self.width() * max_width_percent)
        for label in self.messages_widgets:
            label.setMaximumWidth(new_max_width)
        super().resizeEvent(event)

    def add_message(self, content: str, timestamp: str, msg_id: int):
        sub_bubble = QWidget()
        sub_bubble_layout = QVBoxLayout(sub_bubble)
        sub_bubble_layout.setContentsMargins(10, 4, 10, 4)
        sub_bubble_layout.setSpacing(2)

        message_label = QLabel(content)
        message_label.setWordWrap(True)
        message_label.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Minimum)
        fm = QFontMetrics(message_label.font())
        text_height = fm.boundingRect(0, 0, 400, 0, Qt.TextFlag.TextWordWrap, content).height()
        line_height = fm.lineSpacing()
        message_label.setMinimumHeight(text_height + line_height)

        timestamp_label = QLabel(timestamp)
        timestamp_label.setStyleSheet("color: gray; font-size: 10px;")
        timestamp_label.setAlignment(Qt.AlignmentFlag.AlignRight)

        sub_bubble_layout.addWidget(message_label)
        sub_bubble_layout.addWidget(timestamp_label)

        # Custom menu
        sub_bubble.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
        sub_bubble.customContextMenuRequested.connect(lambda pos, mid=msg_id: self.show_context_menu(pos, mid))

        bubble_container = self.main_layout.itemAt(1 if self.main_layout.itemAt(0).spacerItem() else 0).widget()
        bubble_layout = bubble_container.layout()
        bubble_layout.addWidget(sub_bubble)

        self.message_ids.append(msg_id)
        self.adjustSize()

    def show_context_menu(self, pos, message_id):
        if self.parent_window:
            self.parent_window.show_message_context_menu(self.mapToGlobal(pos), message_id)


class TelegramWindow(QMainWindow):
    def __init__(self, client: TelegramClient, manager: TelegramChatManager, loop):
        super().__init__()
        self.client = client
        self.manager: TelegramChatManager = manager
        self.loop = loop
        self.me = None
        self.refresh_timer = QTimer()
        self.refresh_timer.timeout.connect(self.check_new_messages)
        self.refresh_timer.start(2000)

        self.upd_chats_timer = QTimer()
        def upd_chats():
            self.dialogs = asyncio.run_coroutine_threadsafe(self.client.get_dialogs(), self.loop).result()
        self.upd_chats_timer.timeout.connect(upd_chats)
        self.upd_chats_timer.start(2000)

        self.dialogs = asyncio.run_coroutine_threadsafe(self.client.get_dialogs(), self.loop).result()

        self.setWindowTitle("TeleForge")
        self.setGeometry(300, 300, 800, 600)

        self.last_username = None

        # Load me
        future = asyncio.run_coroutine_threadsafe(self.client.get_me(), self.loop)
        self.me = future.result()

        main_widget = QWidget()
        self.setCentralWidget(main_widget)
        main_layout = QHBoxLayout(main_widget)
        main_layout.setContentsMargins(0, 0, 0, 0)
        main_layout.setSpacing(0)

        # --- SIDEBAR ---
        sidebar = QWidget()
        sidebar.setFixedWidth(300)
        sidebar_layout = QVBoxLayout(sidebar)
        sidebar_layout.setContentsMargins(10, 10, 10, 10)
        sidebar_layout.setSpacing(5)

        # --- HEADER (заголовок + поиск) ---
        header = QWidget()
        header_layout = QVBoxLayout(header)  # вертикальный layout

        # Заголовок
        header_label = QLabel("TeleForge")
        header_label.setStyleSheet("font-size: 16px; font-weight: bold; color: #FFFFFF;")
        header_layout.addWidget(header_label)

        # Поиск
        label_find = QLabel("Поиск чата по имени:")
        line_edit_find_chat = QLineEdit()

        label_find.setStyleSheet("font-size: 14px; font-weight: bold; color: #FFFFFF;")
        line_edit_find_chat.setStyleSheet("font-size: 14px; font-weight: bold; color: #FFFFFF;")

        search_layout = QHBoxLayout()
        search_layout.addWidget(label_find)
        search_layout.addWidget(line_edit_find_chat)
        header_layout.addLayout(search_layout)

        # при изменении текста вызываем функцию
        line_edit_find_chat.textChanged.connect(self.load_chats_with_text)

        sidebar_layout.addWidget(header)

        # --- СПИСОК ЧАТОВ ---
        self.chat_list = QListWidget()
        self.chat_list.setVerticalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        self.chat_list.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        self.chat_list.itemClicked.connect(self.load_chat_messages)
        self.load_chats()

        sidebar_layout.addWidget(self.chat_list)

        # --- CHAT AREA ---
        chat_area = QWidget()
        chat_layout = QVBoxLayout(chat_area)
        chat_layout.setContentsMargins(10, 10, 10, 10)
        chat_layout.setSpacing(5)

        self.chat_header = QWidget()
        chat_header_layout = QHBoxLayout(self.chat_header)
        self.chat_name = QLabel("Select a Chat")

        self.chat_name.setStyleSheet("font-size: 14px; font-weight: bold; color: #FFFFFF;")
        self.chat_status = QLabel("")
        self.chat_status.setStyleSheet("font-size: 12px; color: #FFB84D;")
        chat_header_layout.addWidget(self.chat_name)
        chat_header_layout.addWidget(self.chat_status)
        chat_header_layout.addStretch()
        chat_layout.addWidget(self.chat_header)

        self.scroll_area = QScrollArea()
        self.scroll_area.setWidgetResizable(True)
        self.scroll_area.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        self.scroll_area.setVerticalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAsNeeded)

        self.messages_container = QWidget()
        self.messages_layout = QVBoxLayout(self.messages_container)
        self.messages_layout.setContentsMargins(0, 0, 0, 0)
        self.messages_layout.setSpacing(8)
        self.scroll_area.setWidget(self.messages_container)

        chat_layout.addWidget(self.scroll_area)

        input_area = QWidget()
        input_layout = QHBoxLayout(input_area)
        self.message_input = QPlainTextEdit()
        self.message_input.setFixedHeight(40)
        self.message_input.setLineWrapMode(QPlainTextEdit.LineWrapMode.NoWrap)
        self.message_input.setVerticalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        self.message_input.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        self.message_input.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Fixed)
        self.send_button = QPushButton()
        self.send_button.setIcon(QIcon("send_icon.png"))
        self.send_button.setFixedSize(40, 40)
        self.send_button.clicked.connect(self.send_message)
        input_layout.addWidget(self.message_input)
        input_layout.addWidget(self.send_button)
        chat_layout.addWidget(input_area)

        # --- MAIN SPLIT ---
        main_layout.addWidget(sidebar)
        main_layout.addWidget(chat_area)

        # Стили
        self.setStyleSheet("""
            QWidget {
                background-color: #18222D;
                font-family: 'Arial';
                color: #FFFFFF;
            }
            QMainWindow {
                background-color: #18222D;
            }
            QWidget#sidebar {
                background-color: #1F2A3C;
                border-right: 1px solid #2A2F3B;
            }
            QListWidget {
                background-color: #1F2A3C;
                border: none;
            }
            QListWidget::item {
                padding: 12px;
                border-bottom: 1px solid #2A2F3B;
                color: #FFFFFF;
            }
            QListWidget::item:selected {
                background-color: #3390FF;
                color: #FFFFFF;
            }
            QPushButton#send_button {
                background-color: #3390FF;
                border: none;
                border-radius: 20px;
            }
            QPushButton#send_button:hover {
                background-color: #2678CC;
            }
        """)

        sidebar.setObjectName("sidebar")
        self.send_button.setObjectName("send_button")

        # Connect scroll signal
        self.scroll_area.verticalScrollBar().valueChanged.connect(self.on_scroll)

    def load_chats_with_text(self, text):
        dialogs = self.dialogs
        self.chat_list.clear()

        new_dialogs = []
        for dialog in dialogs:
            title = dialog.title.lower()
            username = getattr(dialog.entity, 'username', "")
            chat_id = dialog.id
            if text.lower() in title or text.lower() in username.lower() if type(username) == str else "":
                new_dialogs.append(dialog)
                item = QListWidgetItem(title)
                item.setData(Qt.ItemDataRole.UserRole, chat_id)
                self.chat_list.addItem(item)

        dialogs = new_dialogs.copy()
        if not dialogs:
            item = QListWidgetItem("Кажется ничего нет 😕")
            self.chat_list.addItem(item)


    def load_chats(self):
        dialogs = self.dialogs
        self.chat_list.clear()

        for dialog in dialogs:
            title = dialog.name
            chat_id = dialog.id
            item = QListWidgetItem(title)
            item.setData(Qt.ItemDataRole.UserRole, chat_id)
            self.chat_list.addItem(item)

        if not dialogs:
            item = QListWidgetItem("Кажется ничего нет 😕")
            self.chat_list.addItem(item)

    def check_new_messages(self):
        if hasattr(self, "current_chat_id"):
            self.load_messages_batch(direction="newer", limit=50)

    def load_chat_messages(self, item):

        self.chat_name.setText(item.text())
        self.chat_status.setText("Loading...")
        # Clear messages
        while self.messages_layout.count():
            w = self.messages_layout.takeAt(0).widget()
            if w:
                w.setParent(None)
        self.current_chat_id = item.data(Qt.ItemDataRole.UserRole)
        asyncio.run_coroutine_threadsafe(self.manager.save_chat_history(self.current_chat_id, 200), self.loop).result()

        self.chat_status.setText("N/A")

        self.min_loaded_id = None
        self.max_loaded_id = None
        self.last_username = None

        # Initial load of last 50
        self.load_messages_batch(direction="older", limit=50, scroll_to_bottom=True)

    def load_messages_batch(self, direction="older", limit=50, scroll_to_bottom=False):
        rows = []
        min_id = self.min_loaded_id if direction == "older" else None
        max_id = self.max_loaded_id if direction == "newer" else None
        future = asyncio.run_coroutine_threadsafe(
            self.manager.get_messages_for_batch(self.current_chat_id, direction, min_id, max_id, limit),
            self.loop
        )
        rows = future.result()

        if not rows:
            return

        # Update min/max loaded ids
        new_ids = [row[0] for row in rows]
        if self.min_loaded_id is None:
            self.min_loaded_id = min(new_ids)
            self.max_loaded_id = max(new_ids)
        else:
            if direction == "older":
                self.min_loaded_id = min(new_ids)
            elif direction == "newer":
                self.max_loaded_id = max(new_ids)

        # Group messages
        if direction == "older":
            rows = rows[::-1]  # ASC for grouping

        grouped = []
        current_group = None
        prev_username = None
        prev_timestamp = None

        for msg_id, content, username, created_at, sender_id in rows:
            timestamp = datetime.datetime.fromtimestamp(created_at).strftime("%H:%M")
            is_own = sender_id == self.me.id
            if current_group and username == prev_username and created_at - prev_timestamp <= 300:
                current_group.append((content, timestamp, msg_id))
            else:
                if current_group:
                    grouped.append((prev_username, current_group, is_own))
                current_group = [(content, timestamp, msg_id)]
                prev_username = username
            prev_timestamp = created_at

        if current_group:
            grouped.append((prev_username, current_group, is_own))

        if direction == "older":
            grouped = grouped[::-1]

        # Save scroll position
        scrollbar = self.scroll_area.verticalScrollBar()
        old_value = scrollbar.value()
        old_max = scrollbar.maximum()

        # Add groups
        if direction == "older":
            for username, msgs, is_own in grouped:
                group = MessageGroupWidget(username, msgs, is_own, parent=self)
                self.messages_layout.insertWidget(0, group)
        elif direction == "newer":
            for username, msgs, is_own in grouped:
                group = MessageGroupWidget(username, msgs, is_own, parent=self)
                self.messages_layout.addWidget(group)

        # Limit number of widgets (~100 groups)
        total_widgets = self.messages_layout.count()
        max_widgets = 100
        if total_widgets > max_widgets:
            if direction == "older":
                for _ in range(total_widgets - max_widgets):
                    w = self.messages_layout.takeAt(self.messages_layout.count() - 1).widget()
                    if w:
                        w.setParent(None)
                # Update max_loaded_id
                self.max_loaded_id = max(
                    [
                        max(self.messages_layout.itemAt(i).widget().message_ids)
                        for i in range(self.messages_layout.count())
                    ]
                )
            elif direction == "newer":
                for _ in range(total_widgets - max_widgets):
                    w = self.messages_layout.takeAt(0).widget()
                    if w:
                        w.setParent(None)
                # Update min_loaded_id
                self.min_loaded_id = min(
                    [
                        min(self.messages_layout.itemAt(i).widget().message_ids)
                        for i in range(self.messages_layout.count())
                    ]
                )

        # Adjust scroll
        def adjust_scroll():
            if scroll_to_bottom:
                scrollbar.setValue(scrollbar.maximum())
            elif direction == "older":
                scrollbar.setValue(old_value + (scrollbar.maximum() - old_max))
            # For newer, if was at bottom, stay at bottom
            elif direction == "newer" and old_value >= old_max - 50:  # Near bottom
                scrollbar.setValue(scrollbar.maximum())

        QTimer.singleShot(0, adjust_scroll)

    def on_scroll(self, value):
        scrollbar = self.scroll_area.verticalScrollBar()
        threshold = 200

        if value <= threshold and self.min_loaded_id > 1:
            self.load_messages_batch(direction="older", limit=50)

        if value >= scrollbar.maximum() - threshold:
            self.load_messages_batch(direction="newer", limit=50)

    def send_message(self):
        message = self.message_input.toPlainText().strip()
        if not message or not self.chat_list.currentItem():
            return

        chat_id = self.chat_list.currentItem().data(Qt.ItemDataRole.UserRole)

        async def send():
            sent_message = await self.client.send_message(chat_id, message)
            await self.manager.save_message(
                chat_id,
                sent_message.from_id.user_id if sent_message.from_id else 0,
                sent_message.id,
                message,
                int(sent_message.date.timestamp()),
                message_type="text",
            )

        future = asyncio.run_coroutine_threadsafe(send(), self.loop)
        future.result()
        self.message_input.clear()

    def show_message_context_menu(self, pos, message_id):
        menu = QMenu(self)
        copy_action = QAction("Copy", self)
        copy_action.triggered.connect(lambda: self.copy_message(message_id))
        reply_action = QAction("Reply", self)
        reply_action.triggered.connect(lambda: self.reply_to_message(message_id))
        delete_action = QAction("Delete", self)
        delete_action.triggered.connect(lambda: self.delete_message(message_id))
        menu.addAction(copy_action)
        menu.addAction(reply_action)
        menu.addAction(delete_action)
        menu.exec(pos)

    def copy_message(self, message_id):
        future = asyncio.run_coroutine_threadsafe(
            self.manager.get_message_content(self.current_chat_id, message_id), self.loop
        )
        content = future.result()
        if content:
            QApplication.clipboard().setText(content)

    def reply_to_message(self, message_id):
        # Placeholder for reply
        print(f"Reply to {message_id}")

    def delete_message(self, message_id):
        created_at = int(time.time())
        future = asyncio.run_coroutine_threadsafe(
            self.manager.delete_message(self.current_chat_id, message_id, created_at), self.loop
        )
        future.result()

        # Find and remove widget
        for i in range(self.messages_layout.count()):
            group = self.messages_layout.itemAt(i).widget()
            if isinstance(group, MessageGroupWidget) and message_id in group.message_ids:
                idx = group.message_ids.index(message_id)
                bubble_container = group.main_layout.itemAt(
                    1 if group.main_layout.itemAt(0).spacerItem() else 0
                ).widget()
                bubble_layout = bubble_container.layout()
                sub = bubble_layout.takeAt(idx).widget()
                if sub:
                    sub.setParent(None)
                group.message_ids.pop(idx)
                if not group.message_ids:
                    group.setParent(None)
                    self.messages_layout.removeWidget(group)
                break

    def closeEvent(self, event):
        super().closeEvent(event)
