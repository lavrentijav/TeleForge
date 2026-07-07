# TeleForge built-in test plugin (shipped next to the executable in plugins/).
#
# It is loaded automatically on startup when Python is enabled and serves as the
# reference example for plugin authors. It shows the three core capabilities:
#   1. talking to the app (settings/app/messages),
#   2. subscribing to core events (message hooks),
#   3. contributing UI (a clickable menu button).
#
# Every hook runs on the dedicated plugin thread, so slow handlers never freeze
# the TeleForge interface.
from __future__ import annotations

import teleforge as tf


def _banner() -> str:
    api_ok = "yes" if tf.available() else "no"
    return f"test_plugin: api={api_ok}, working_dir={tf.app.working_dir()}"


tf.app.log(_banner())


# --- AI-callable tool ------------------------------------------------------
@tf.tool("teleforge_ping")
def teleforge_ping() -> str:
    """AI-callable sanity check: returns a short status string."""
    return _banner()


# --- Event hooks -----------------------------------------------------------
@tf.on_message
def _on_incoming(event: dict):
    """Called for every incoming message. Returning a str rewrites its text."""
    tf.app.log(
        "test_plugin: incoming message"
        f" id={event.get('message_id')} text={event.get('text', '')!r}")
    # Uncomment to rewrite the displayed text of incoming messages:
    # return event.get("text", "").upper()


@tf.on_message_out
def _on_outgoing(event: dict):
    """Called for every outgoing message before it is delivered."""
    tf.app.log(f"test_plugin: outgoing text={event.get('text', '')!r}")


@tf.on_user_status
def _on_status(event: dict):
    tf.app.log(f"test_plugin: user status changed {event!r}")


# --- Custom UI -------------------------------------------------------------
def _hello_button() -> None:
    tf.app.log("test_plugin: menu button clicked!")


tf.ui.add_menu_item(
    "Test plugin: say hello",
    _hello_button,
    item_id="test_hello")
