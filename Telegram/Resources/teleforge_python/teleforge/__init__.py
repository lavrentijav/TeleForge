"""TeleForge plugin API.

This package is the friendly, documented Python interface that plugins use to
talk to the running TeleForge application. It wraps the native ``_teleforge``
module that the app injects into the embedded interpreter.

Example
-------
::

    import teleforge as tf

    # Read and tweak any setting (everything changeable lives here).
    tf.settings.set("disableAds", True)
    print(tf.settings.get("appIcon"))

    # Send a message to a known peer.
    tf.messages.send(peer_id, "hello from a plugin")

    # Sensitive actions always ask the user for confirmation first.
    tf.account.change_password()

Everything that the app can change is exposed through :data:`settings`.
Security-sensitive actions (password changes, registration, logging in new
sessions, terminating sessions) never run silently: they pop a confirmation
dialog and only proceed if the user explicitly agrees.
"""

from __future__ import annotations

import json as _json
from typing import Any, Callable, Optional

try:
    import _teleforge as _native
except ImportError:  # Running outside the app (e.g. tooling / tests).
    _native = None


__all__ = [
    "settings",
    "app",
    "messages",
    "account",
    "tool",
    "available",
    "ui",
    "on_message",
    "on_message_out",
    "on_user_status",
    "on_chat_list",
    "on_media",
    "on_event",
]


def available() -> bool:
    """Return ``True`` when running inside the TeleForge application."""
    return _native is not None


def _require_native():
    if _native is None:
        raise RuntimeError(
            "teleforge API is only available inside the TeleForge app."
        )
    return _native


class _Settings:
    """Access to every changeable application setting.

    Settings are exposed as plain JSON-compatible Python values. Use
    :meth:`all` to inspect the full configuration, then :meth:`get`/:meth:`set`
    or :meth:`update` to change individual keys. Changes are persisted
    immediately.
    """

    def all(self) -> dict:
        """Return the whole settings tree as a dict."""
        return _json.loads(_require_native().settings_all())

    def get(self, key: str, default: Any = None) -> Any:
        """Return the value of one setting, or ``default`` if unset."""
        raw = _require_native().settings_get(key)
        if raw is None:
            return default
        return _json.loads(raw)

    def set(self, key: str, value: Any) -> bool:
        """Set one setting to ``value`` and persist it."""
        return _require_native().settings_set(key, _json.dumps(value))

    def update(self, values: dict) -> bool:
        """Merge a dict of settings over the current configuration."""
        return _require_native().settings_apply(_json.dumps(values))

    def reset(self) -> None:
        """Reset all settings back to their defaults."""
        _require_native().settings_reset()

    def __getitem__(self, key: str) -> Any:
        value = self.get(key, _MISSING)
        if value is _MISSING:
            raise KeyError(key)
        return value

    def __setitem__(self, key: str, value: Any) -> None:
        self.set(key, value)


class _App:
    """Application-level utilities."""

    def working_dir(self) -> str:
        """Return the app working directory (where tdata lives)."""
        return _require_native().app_working_dir()

    def log(self, message: str) -> None:
        """Write a line to the application log."""
        _require_native().app_log(str(message))


class _Messages:
    """Send messages on behalf of the active account."""

    def send(self, peer_id: int, text: str) -> bool:
        """Send ``text`` to ``peer_id`` (a serialized peer id)."""
        return _require_native().send_message(int(peer_id), str(text))


class _Account:
    """Security-sensitive account operations.

    Each method here triggers a hard confirmation dialog ("Точно это нужно?")
    and only proceeds if the user agrees. They never execute silently.
    """

    def change_password(self) -> None:
        """Ask the user to confirm changing the 2FA password."""
        _require_native().account_change_password()

    def register(self) -> None:
        """Ask the user to confirm registering a new account."""
        _require_native().account_register()

    def new_session(self) -> None:
        """Ask the user to confirm logging in a new session."""
        _require_native().account_new_session()

    def terminate_session(self) -> None:
        """Ask the user to confirm terminating a session."""
        _require_native().account_terminate_session()


class _MissingType:
    pass


_MISSING = _MissingType()

settings = _Settings()
app = _App()
messages = _Messages()
account = _Account()

# Registry of plugin-provided tools the AI can call.
_tools: dict[str, Callable[..., Any]] = {}


def tool(name: Optional[str] = None) -> Callable[[Callable[..., Any]], Callable[..., Any]]:
    """Decorator registering a function as an AI-callable tool.

    ::

        @tf.tool()
        def summarize(chat_id: int) -> str:
            ...
    """

    def decorator(func: Callable[..., Any]) -> Callable[..., Any]:
        _tools[name or func.__name__] = func
        return func

    return decorator


# ---------------------------------------------------------------------------
# Event hooks
# ---------------------------------------------------------------------------
#
# Plugins subscribe to core TeleForge events with the decorators below. Every
# handler runs on the dedicated plugin thread, so a slow handler never freezes
# the interface. Handlers receive a single ``event`` dict; message handlers may
# return a modified text (str) or a dict to override fields of the event.

# event name -> list of handlers
_hooks: dict[str, list[Callable[[dict], Any]]] = {}


def on_event(name: str) -> Callable[[Callable[[dict], Any]], Callable[[dict], Any]]:
    """Subscribe a handler to an arbitrary named core event."""

    def decorator(func: Callable[[dict], Any]) -> Callable[[dict], Any]:
        _hooks.setdefault(name, []).append(func)
        return func

    return decorator


def on_message(func: Callable[[dict], Any]) -> Callable[[dict], Any]:
    """Handle an incoming message. Return a str to rewrite its displayed text."""
    return on_event("message_in")(func)


def on_message_out(func: Callable[[dict], Any]) -> Callable[[dict], Any]:
    """Handle an outgoing message before it is sent."""
    return on_event("message_out")(func)


def on_user_status(func: Callable[[dict], Any]) -> Callable[[dict], Any]:
    """Handle a user online/offline/typing status change."""
    return on_event("user_status")(func)


def on_chat_list(func: Callable[[dict], Any]) -> Callable[[dict], Any]:
    """Handle a chat-list update."""
    return on_event("chat_list")(func)


def on_media(func: Callable[[dict], Any]) -> Callable[[dict], Any]:
    """Handle a media load/download event."""
    return on_event("media")(func)


# ---------------------------------------------------------------------------
# UI registration
# ---------------------------------------------------------------------------

class _Ui:
    """Register custom UI elements contributed by the plugin."""

    def add_menu_item(self, title: str, handler: Callable[[], Any],
                      item_id: Optional[str] = None) -> str:
        """Add a clickable item to the plugins menu. Returns its id."""
        key = item_id or f"item_{len(_menu_items) + 1}_{title}"
        _menu_items[key] = {"title": str(title), "handler": handler}
        return key

    def remove_menu_item(self, item_id: str) -> None:
        _menu_items.pop(item_id, None)


ui = _Ui()

# id -> {"title": str, "handler": callable}
_menu_items: dict[str, dict] = {}


# ---------------------------------------------------------------------------
# Native bridge entry points (called by the C++ host on the plugin thread).
# ---------------------------------------------------------------------------

def _reset() -> None:
    """Clear every registration. Called by the host before reloading plugins."""
    _tools.clear()
    _hooks.clear()
    _menu_items.clear()


def _dispatch_event(name: str, payload_json: str) -> Optional[str]:
    """Run every handler subscribed to ``name``.

    Returns an optional JSON string with fields to override on the event
    (e.g. ``{"text": "..."}``) when a message handler rewrites the message.
    """
    handlers = _hooks.get(name)
    if not handlers:
        return None
    try:
        event = _json.loads(payload_json) if payload_json else {}
    except Exception:
        event = {}
    override: dict = {}
    for handler in list(handlers):
        try:
            result = handler(dict(event))
        except Exception as error:  # A broken plugin must not kill the host.
            if _native is not None:
                _native.app_log(f"plugin hook '{name}' failed: {error!r}")
            continue
        if isinstance(result, str):
            override["text"] = result
            event["text"] = result
        elif isinstance(result, dict):
            override.update(result)
            event.update(result)
    return _json.dumps(override) if override else None


def _menu_items() -> str:
    """Return the registered menu items as a JSON list of {id, title}."""
    return _json.dumps(
        [{"id": key, "title": value["title"]}
         for key, value in _menu_items.items()])


def _invoke_menu(item_id: str) -> None:
    """Invoke the handler of a registered menu item by id."""
    item = _menu_items.get(item_id)
    if not item:
        return
    try:
        item["handler"]()
    except Exception as error:
        if _native is not None:
            _native.app_log(f"menu item '{item_id}' failed: {error!r}")
