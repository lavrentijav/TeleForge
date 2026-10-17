"""Entry point: python -m teleforge_scanner --config config.toml"""

from __future__ import annotations

import argparse
import asyncio
import logging
import sys

from . import ssh_tunnel
from .config import load_config
from .scanner import Scanner


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(prog="teleforge_scanner")
    parser.add_argument("--config", "-c", default="config.toml")
    args = parser.parse_args(argv)

    try:
        cfg = load_config(args.config)
    except FileNotFoundError:
        print(f"config not found: {args.config}", file=sys.stderr)
        return 2

    logging.basicConfig(
        level=getattr(logging, cfg.runtime.log_level.upper(), logging.INFO),
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )

    scanner = Scanner(cfg)
    try:
        asyncio.run(scanner.run())
    except KeyboardInterrupt:
        logging.getLogger("teleforge").info("interrupted, shutting down")
    finally:
        ssh_tunnel.shutdown()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
