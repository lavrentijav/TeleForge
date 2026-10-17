"""Interactive helper: create a StringSession to paste into config.toml.

Run once on a trusted machine:  python -m teleforge_scanner.login
"""

from __future__ import annotations

import argparse

from telethon import TelegramClient
from telethon.sessions import StringSession

from .config import load_config


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(prog="teleforge_scanner.login")
    parser.add_argument("--config", "-c", default="config.toml")
    args = parser.parse_args(argv)

    cfg = load_config(args.config)
    with TelegramClient(
        StringSession(),
        cfg.telegram.api_id,
        cfg.telegram.api_hash,
    ) as client:
        session = client.session.save()
        print("\nAdd this to config.toml under [telegram]:\n")
        print(f'session_string = "{session}"\n')
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
