#!/usr/bin/env bash
# Builds and runs the archive-key test inside the build container.
# Two devices are simulated with separate working directories and processes.
set -eu
here="$(cd "$(dirname "$0")" && pwd)"
src=/usr/src/teleforge/Telegram/SourceFiles/tfc/features/sync/teleforge_sync_crypto.cpp
g++ -std=c++20 -fPIC -O1 -include "$here/stub/prefix.h" "$here/main.cpp" "$src" \
	-I"$here/stub" -I/usr/src/teleforge/Telegram/SourceFiles \
	-I/usr/local/include -I/usr/local/include/QtCore \
	-L/usr/local/lib -lQt6Core -lcrypto -lz -ldl -lrt -lpthread -lglib-2.0 -lpcre2-16 \
	-o /tmp/tf-crypto-test
rm -rf /tmp/tf-device-a /tmp/tf-device-b /tmp/tf-device-bundle.bin
/tmp/tf-crypto-test
/tmp/tf-crypto-test device-write
/tmp/tf-crypto-test device-read
