import asyncio
import queue
import threading
from datetime import datetime
from typing import Any, Generator

from dearpygui import dearpygui as dpg
from decouple import config
from pyrogram import Client, filters
from sqlalchemy import create_engine
from sqlalchemy.orm import sessionmaker

from models import Base, Chat, Message, DeletedMessage, MessageHistory

# Инициализация очередей для синхронизации
to_ui_queue = queue.Queue()  # Очередь для данных от Pyrogram к UI
from_ui_queue = queue.Queue()  # Очередь для команд от UI к Pyrogram

# Конфигурация
api_id = config('API_ID')
api_hash = config('API_HASH')
phone = config('PHONE')
login = config('LOGIN')

RUN = True
splitter_pos = 300

# Настройка SQLAlchemy
engine = create_engine('sqlite:///telegram.db', echo=False)
Base.metadata.create_all(engine)
Session = sessionmaker(bind=engine)
session = Session()

# Pyrogram клиент
app = Client(name=login, api_id=api_id, api_hash=api_hash, phone_number=phone)

import dearpygui.dearpygui as dpg

import contextlib

@contextlib.contextmanager
def align_items(n_cols_left: int, n_cols_right: int) -> Generator[int | str, Any, None]:
	"""
	Adds a table to align items.

	Please note:
	Many items (e.g. combo, drag_*, input_*, slider_*, listbox, progress_bar) will not display unless a positive width is set

	Args:
		n_cols_left: Align n items to the left. (n_cols_left)
		n_cols_right: Align n items to the right (n_cols_right)
	"""
	if n_cols_left < 0 or n_cols_right < 0:
		raise ValueError("Column amount must be 0 or higher")

	table = dpg.add_table(resizable=False, header_row=False, policy=0)
	for _ in range(n_cols_left - 1):
		dpg.add_table_column(width_stretch=False, width_fixed=True, parent=table)
	dpg.add_table_column(width_stretch=False, width_fixed=False, parent=table)
	for _ in range(n_cols_right):
		dpg.add_table_column(width_stretch=False, width_fixed=True, parent=table)
	widget = dpg.add_table_row(parent=table)
	if n_cols_left == 0:
		dpg.add_spacer(parent=widget)

	dpg.push_container_stack(widget)
	try:
		yield widget
	finally:
		dpg.pop_container_stack()


def update_splitter(sender, app_data):
    global splitter_pos
    splitter_pos = app_data[0]  # Обновляем позицию разделителя
    dpg.configure_item("chats_panel", width=splitter_pos)
    dpg.configure_item("messages_panel", width=800 - splitter_pos - 5)  # -5 для отступа

def add_message(chat_id, message_id, text, is_user=True):
    with dpg.group(horizontal=True, tag=f"msg_{message_id}"):
        if is_user:
            dpg.add_spacer(width=32)
            #dpg.add_image("user_icon", width=32, height=32)
            dpg.add_text(text, wrap=300)

        else:
            dpg.add_text(text, wrap=300)
            dpg.add_spacer(width=32)  # Пропуск места для иконки



# Асинхронная функция для обработки событий Pyrogram
async def pyrogram_worker():
    async with app:
        # Обработчик новых сообщений (фильтрация рекламы)
        @app.on_message(filters.all)
        async def handle_message(client, message):
            if "sponsored" in (message.text or "").lower() or message.service:
                return
            session.merge(Chat(chat_id=message.chat.id, title=message.chat.title or message.chat.first_name,
                               last_updated=datetime.now()))
            session.merge(Message(chat_id=message.chat.id, message_id=message.id, text=message.text,
                                  sender_id=message.sender.id if message.sender else None, sent_at=message.date))
            session.commit()
            to_ui_queue.put(("new_message", message.chat.id, message.id, message.text))

        # Обработчик отредактированных сообщений
        @app.on_message(filters.Message.edit)
        async def handle_edited_message(client, message):
            version = session.query(MessageHistory).filter_by(chat_id=message.chat.id,
                                                              message_id=message.id).count() + 1
            session.merge(MessageHistory(chat_id=message.chat.id, message_id=message.id, version=version,
                                         text=message.text, edited_at=message.edit_date))
            session.commit()
            to_ui_queue.put(("message_edited", message.chat.id, message.id, version, message.text))

        # Обработчик удалённых сообщений
        @app.on_raw_update()
        async def handle_raw_update(client, update, users, chats):
            if hasattr(update, "messages") and hasattr(update, "chat_id"):
                for message_id in update.messages:
                    session.merge(DeletedMessage(chat_id=update.chat_id, message_id=message_id,
                                                 text="Unknown text", deleted_at=datetime.now()))
                    session.commit()
                    to_ui_queue.put(("message_deleted", update.chat_id, message_id))

        # Обработка команд от UI
        global RUN
        while RUN:
            try:
                command, *args = from_ui_queue.get_nowait()
                print(command)
                if command == "get_chats":
                    async for dialog in app.get_dialogs():
                        chat = dialog.chat
                        #session.merge(
                        #    Chat(chat_id=chat.id, title=chat.title or chat.first_name, last_updated=datetime.now()))
                        #session.commit()
                        to_ui_queue.put(("add chat", chat.id, chat.title or chat.first_name))
                elif command == "send_message":
                    chat_id, text = args
                    await app.send_message(chat_id, text)
                elif command == "stop":
                    break
            except queue.Empty:
                await asyncio.sleep(0.1)


# Запуск UI в отдельном потоке
def run_ui():
    dpg.create_context()

    with dpg.font_registry():
        with dpg.font("fonts/JetBrainsMono-Regular.ttf", 16, default_font=True, tag="JB_regular_14", pixel_snapH=True) as f:
            dpg.add_font_range_hint(dpg.mvFontRangeHint_Cyrillic)
        with dpg.font("fonts/JetBrainsMono-Bold.ttf", 24, tag="JB_bold_18", pixel_snapH=True) as f:
            dpg.add_font_range_hint(dpg.mvFontRangeHint_Cyrillic)

    dpg.bind_font("JB_regular_14")

    def refresh_chats():
        dpg.delete_item("chat_table", children_only=True)
        from_ui_queue.put(("get_chats",))

    def send_message():
        chat_id = dpg.get_value("selected_chat")
        text = dpg.get_value("message_input")
        if chat_id and text:
            from_ui_queue.put(("send_message", int(chat_id), text))
            dpg.set_value("message_input", "")

    def show_history(chat_id, message_id):
        versions = session.query(MessageHistory).filter_by(chat_id=chat_id, message_id=message_id).all()
        with dpg.window(label=f"History for msg {message_id}", width=400, height=300):
            for version in versions:
                dpg.add_text(f"Version {version.version} ({version.edited_at}): {version.text}")

    def show_chat(*args):
        pass

    def update_ui():
        while True:


            try:
                event, *args = to_ui_queue.get_nowait()
            except queue.Empty:
                break

            if event == "add chat":
                chat_id, name = args
                f = dpg.add_table_row(parent="chats")
                dpg.add_button(parent=f, label=name, callback=show_chat, args=[chat_id], tag=f"button_on_{chat_id}")
            elif event == "new_message":
                chat_id, message_id, text = args
                if str(chat_id) == dpg.get_value("selected_chat"):
                    dpg.add_text(text, parent="chat_window")
            elif event == "message_deleted":
                chat_id, message_id = args
                if str(chat_id) == dpg.get_value("selected_chat"):
                    dpg.add_collapsing_header(label=f"Deleted msg {message_id}", parent="chat_window")
            elif event == "message_edited":
                chat_id, message_id, version, text = args
                if str(chat_id) == dpg.get_value("selected_chat"):
                    dpg.add_button(label=f"View history msg {message_id}", parent="chat_window",
                                   callback=lambda: show_history(chat_id, message_id))


    with dpg.window(label="TeleForge", tag="main_window",
                    width=800, height=600,
                    no_resize=True,
                    no_move=True,
                    no_collapse=True,
                    no_title_bar=True):
        with dpg.tab_bar(label="Categories", tag="tab_bar_categories") as tab_bar:
            dpg.bind_item_font(tab_bar, "JB_bold_18")
            with dpg.tab(label="all dialogs", tag="tab_all") as tab:
                #dpg.bind_item_font(tab, "JB_bold_18")
                with dpg.table(resizable=True, header_row=False):
                    dpg.add_table_column()
                    dpg.add_table_column()
                    with dpg.table_row():
                        with dpg.table(header_row=False, tag="chats"):
                            dpg.add_table_column()
                            with dpg.table_row():
                                title = dpg.add_text("Чаты")
                                dpg.bind_item_font(title, "JB_bold_18")

                        with dpg.table(header_row=False):
                            dpg.add_table_column()
                            with dpg.table_row():
                                title = dpg.add_text("Диалог с <123>")
                                dpg.bind_item_font(title, "JB_bold_18")


                            with dpg.table_row():
                                with dpg.child_window(tag="messages_panel"):
                                    add_message(1, 1, "Hello from user!", is_user=True)  # Сообщение от пользователя
                                    add_message(1, 2, "Hi, this is me!", is_user=False)  # Твоё сообщение


    dpg.create_viewport(title="TeleForge", width=800, height=600)
    dpg.setup_dearpygui()
    dpg.show_viewport()

    from_ui_queue.put(("get_chats", ))

    while dpg.is_dearpygui_running():
        v_height, v_width = dpg.get_viewport_height(), dpg.get_viewport_width()
        w_height, w_width = dpg.get_item_height("main_window"), dpg.get_item_width("main_window")
        if v_height != w_height + 16 or v_width != w_width + 39:
            dpg.set_item_height("main_window", v_height - 39)
            dpg.set_item_width("main_window", v_width - 16)
            pass
        update_ui()
        dpg.render_dearpygui_frame()
    global RUN
    RUN = False

    dpg.destroy_context()


# Запуск Pyrogram в главном потоке и UI во втором
async def main():
    ui_thread = threading.Thread(target=run_ui, daemon=True)
    ui_thread.start()
    await pyrogram_worker()


if __name__ == "__main__":
    asyncio.run(main())
