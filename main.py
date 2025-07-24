import asyncio
import contextlib
import queue
import threading
from datetime import datetime
from typing import Any, Generator

from dearpygui import dearpygui as dpg
from decouple import config
from pyrogram import Client, filters
from pyrogram.enums import MessagesFilter
from pyrogram.raw import functions, types
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


# Асинхронная функция для обработки событий Pyrogram
async def pyrogram_worker():
    # Pyrogram клиент
    app = Client(name=login, api_id=api_id, api_hash=api_hash,
                 phone_number=phone)  # Обработчик новых сообщений (фильтрация рекламы)

    @app.on_message(filters.all)
    async def handle_message(client, message):
        if "sponsored" in (message.text or "").lower() or message.service:
            return
        session.merge(Chat(chat_id=message.chat.id, title=message.chat.title or message.chat.first_name,
                           last_updated=datetime.now()))
        session.merge(Message(chat_id=message.chat.id, message_id=message.id, text=message.text,
                              sender_id=message.sender.id if hasattr(message, "sender") else None,
                              sent_at=message.date))
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
    async with app:

        async def fetch_dialogs(folder_id=None, include_pinned=True, only_chats=False):

            result = await app.invoke(

                functions.messages.GetDialogs(

                    offset_date=0,

                    offset_id=0,

                    offset_peer=types.InputPeerEmpty(),

                    limit=100,

                    hash=0,

                    folder_id=folder_id

                )

            )

            # id -> user/chat

            all_entities = {}

            for user in result.users:
                all_entities[user.id] = user

            for chat in result.chats:
                all_entities[chat.id] = chat

            output = []

            for dialog in result.dialogs:

                if not include_pinned and getattr(dialog, "pinned", False):
                    continue

                peer = dialog.peer

                if isinstance(peer, types.PeerUser):

                    if only_chats:
                        continue

                    entity = all_entities.get(peer.user_id)

                elif isinstance(peer, types.PeerChat):

                    entity = all_entities.get(peer.chat_id)

                elif isinstance(peer, types.PeerChannel):

                    entity = all_entities.get(peer.channel_id)

                else:

                    continue

                if not entity:
                    continue

                if isinstance(entity, types.User):

                    title = f"{entity.first_name or ''} {entity.last_name or ''}".strip()

                else:

                    title = entity.title

                output.append(title)

            return output

        async def get_chats_from_filter(app: Client, dialog_filter):
            peers = dialog_filter.include_peers or []
            chats = []

            for peer in peers:
                title = ""
                peer_id = 0
                username = ""
                try:
                    if isinstance(peer, types.InputPeerUser):
                        full = await app.invoke(functions.users.GetFullUser(id=peer))
                        title = full.users[0].first_name
                        peer_id = full.users[0].id
                        username = full.users[0].username
                    elif isinstance(peer, types.InputPeerChannel):
                        full = await app.invoke(functions.channels.GetFullChannel(channel=peer))
                        print(full)
                        continue

                    else:
                        print("unknown peer type:", type(peer))
                        continue

                    chats.append({
                        'id': peer_id,
                        'title': title,
                        'username': username
                    })
                except Exception as e:
                    print(f"❌ Не удалось получить чат {peer}: {e}")
            return chats
        while RUN:

            try:
                command, *args = from_ui_queue.get_nowait()

                if command == "all_chats":
                    to_ui_queue.put(("result_all_chats", await fetch_dialogs(folder_id=0)))

                elif command == "all_chats_no_pinned":
                    to_ui_queue.put(
                        ("result_all_chats_no_pinned", await fetch_dialogs(folder_id=0, include_pinned=False)))

                elif command == "archived":
                    to_ui_queue.put(("result_archived", await fetch_dialogs(folder_id=1)))

                elif command == "archived_no_pinned":
                    to_ui_queue.put(
                        ("result_archived_no_pinned", await fetch_dialogs(folder_id=1, include_pinned=False)))

                elif command == "only_chats":
                    to_ui_queue.put(("result_only_chats", await fetch_dialogs(folder_id=0, only_chats=True)))

                elif command == "archived_chats":
                    to_ui_queue.put(("result_archived_chats", await fetch_dialogs(folder_id=1, only_chats=True)))

                elif command == "archived_chats_no_pinned":
                    to_ui_queue.put(("result_archived_chats_no_pinned",
                                     await fetch_dialogs(folder_id=1, only_chats=True, include_pinned=False)))

                elif command == "all_chats_no_pinned":
                    to_ui_queue.put(("result_all_chats_no_pinned",
                                     await fetch_dialogs(folder_id=0, only_chats=True, include_pinned=False)))

                elif command == "get_user_info":
                    user_id = args[0]
                    user = await app.get_users(user_id)
                    to_ui_queue.put(("result_get_user_info", {
                        "id": user.id,
                        "username": user.username,
                        "first_name": user.first_name,
                        "last_name": user.last_name,
                        "phone_number": user.phone_number,
                        "is_bot": user.is_bot,
                        "dc_id": user.dc_id,
                        "is_premium": getattr(user, "is_premium", False)
                    }))

                elif command == "get_chat_info":
                    chat_id = args[0]
                    chat = await app.get_chat(chat_id)
                    to_ui_queue.put(("result_get_chat_info", {
                        "id": chat.id,
                        "title": chat.title,
                        "type": chat.type,
                        "username": chat.username,
                        "description": chat.description,
                        "members_count": chat.members_count,
                        "dc_id": chat.dc_id
                    }))

                elif command == "get_profile_photo":
                    target_id = args[0]
                    photos = await app.get_profile_photos(target_id)
                    if photos:
                        path = await app.download_media(photos[0])
                        to_ui_queue.put(("result_get_profile_photo", {"path": path}))
                    else:
                        to_ui_queue.put(("result_get_profile_photo", {"error": "No profile photos"}))

                elif command == "get_message":
                    chat_id = args[0]
                    message_id = int(args[1])
                    msg = await app.get_messages(chat_id, message_ids=message_id)
                    to_ui_queue.put(("result_get_message", {"text": msg.text or "<no text>", "id": msg.id}))

                elif command == "download_file":
                    chat_id = args[0]
                    message_id = int(args[1])
                    msg = await app.get_messages(chat_id, message_ids=message_id)
                    if msg.media:
                        path = await app.download_media(msg)
                        to_ui_queue.put(("result_download_file", {"path": path}))
                    else:
                        to_ui_queue.put(("result_download_file", {"error": "No media in message"}))

                elif command == "search_media":
                    chat_id = args[0]
                    files = []
                    async for msg in app.search_messages(chat_id, filter=MessagesFilter.DOCUMENT):
                        if msg.document:
                            files.append({"name": msg.document.file_name, "id": msg.id})
                    to_ui_queue.put(("result_search_media", files))

                elif command == "get_me":
                    result = await app.get_me()
                    to_ui_queue.put(("result_get_me", result))

                elif command == "get_chat":
                    chat_id = args[0]
                    result = await app.get_chat(chat_id)
                    to_ui_queue.put(("result_get_chat", result))

                elif command == "get_chat_files":
                    chat_id = args[0]
                    limit = int(args[1]) if len(args) > 1 else 100
                    files = []
                    async for message in app.search_messages(chat_id, filter=MessagesFilter.DOCUMENT, limit=limit):
                        if message.document:
                            files.append({
                                "file_name": message.document.file_name,
                                "file_size": message.document.file_size,
                                "file_id": message.document.file_id,
                                "date": message.date,
                            })
                    to_ui_queue.put(("result_get_chat_files", files))

                elif command == "all_folders":
                    dialog_filters = await app.invoke(functions.messages.GetDialogFilters())

                    folders = {}
                    for folder in dialog_filters:
                        if type(folder) == types.DialogFilterDefault:
                            continue

                        folders.update({folder.ID: {"title": folder.title, "id": folder.id,
                                                    "chats": await get_chats_from_filter(app, folder)}})

                    print(folders)

                    to_ui_queue.put(("result_all_folders", folder))


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
        with dpg.font("fonts/JetBrainsMono-Regular.ttf", 16, default_font=True, tag="JB_regular_14",
                      pixel_snapH=True) as f:
            dpg.add_font_range_hint(dpg.mvFontRangeHint_Cyrillic)
        with dpg.font("fonts/JetBrainsMono-Bold.ttf", 24, tag="JB_bold_24", pixel_snapH=True) as f:
            dpg.add_font_range_hint(dpg.mvFontRangeHint_Cyrillic)
        with dpg.font("fonts/JetBrainsMono-Regular.ttf", 18, default_font=True, tag="JB_regular_18",
                      pixel_snapH=True) as f:
            dpg.add_font_range_hint(dpg.mvFontRangeHint_Cyrillic)

    with dpg.theme(tag="left_aligned_theme"):
        with dpg.theme_component(dpg.mvButton):
            dpg.add_theme_style(dpg.mvStyleVar_CellPadding, 5, 5)  # Отступы слева
            dpg.add_theme_style(dpg.mvStyleVar_FramePadding, 5, 5)  # Уменьшаем внутренние отступы
        with dpg.theme_component(dpg.mvSelectable):
            dpg.add_theme_style(dpg.mvStyleVar_CellPadding, 5, 5)
            dpg.add_theme_style(dpg.mvStyleVar_FramePadding, 5, 5)

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

    def show_chat(chat_id: int):
        pass

    selected_chat_id = None

    def load_messages(chat_id):
        global selected_chat_id
        if selected_chat_id:
            dpg.set_value(selected_chat_id, False)  # Снимаем выделение с предыдущего
        selected_chat_id = chat_id
        dpg.set_value(chat_id, True)  # Выделяем текущий чат

    def update_ui():
        while True:

            try:
                event, *args = to_ui_queue.get_nowait()
            except queue.Empty:
                break

            if event == "result_all_folders":
                print(args)

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
                    no_title_bar=True,
                    ):
        dpg.set_primary_window("main_window", True)

        with dpg.table(resizable=True, header_row=False):
            dpg.add_table_column()
            dpg.add_table_column()
            with dpg.table_row():
                with dpg.tab_bar(label="Categories", tag="tab_bar_categories") as tab_bar:
                    dpg.bind_item_font(tab_bar, "JB_bold_24")
                    with dpg.tab(label="all dialogs", tag="tab_all") as tab:
                        # dpg.bind_item_font(tab, "JB_bold_24")
                        with dpg.child_window():
                            with dpg.table(header_row=False, tag="chats"):
                                dpg.add_table_column(width_stretch=True)
                                with dpg.table_row():
                                    title = dpg.add_text("Чаты")
                                    dpg.bind_item_font(title, "JB_bold_24")
                                """for chat_id, chat_name in chats_data:
                                    """
                                with dpg.table_row():
                                    with dpg.tree_node(label="Архив", default_open=False):
                                        """for chat_id, chat_name in archived_chats_data:
                                            sel = dpg.add_selectable(label=chat_name,
                                                                     callback=lambda: load_messages(chat_id),
                                                                     user_data=chat_id)
                                            dpg.bind_item_theme(sel, "left_aligned_theme")"""

                with dpg.table(header_row=False):
                    dpg.add_table_column()
                    with dpg.table_row():
                        title = dpg.add_text("Диалог с <123>")
                        dpg.bind_item_font(title, "JB_bold_24")

                    with dpg.table_row():
                        with dpg.child_window(tag="messages_panel"):
                            pass
    dpg.create_viewport(title="TeleForge", width=800, height=600)
    dpg.setup_dearpygui()
    dpg.show_viewport()

    from_ui_queue.put(("all_folders",))

    while dpg.is_dearpygui_running():
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
