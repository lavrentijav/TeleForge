# TeleForge built-in test plugin (shipped next to the executable in plugins/).
# Loaded automatically on startup when Python is enabled.
from __future__ import annotations

import teleforge as tf


def _banner() -> str:
    wd = tf.app.working_dir()
    api_ok = "yes" if tf.available() else "no"
    return f"test_plugin: api={api_ok}, working_dir={wd}"


tf.app.log(_banner())


@tf.tool("teleforge_ping")
def teleforge_ping() -> str:
    """AI-callable sanity check: returns a short status string."""
    return _banner()


# Uncomment to verify settings access (writes are persisted immediately):
# tf.settings.set("disableAds", tf.settings.get("disableAds", False))
