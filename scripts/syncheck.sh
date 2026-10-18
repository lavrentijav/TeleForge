#!/bin/bash
# Точечная проверка компиляции изменённых файлов с реальными флагами из build.ninja.
set +e

cd /usr/src/teleforge/out || exit 1

SOURCES="
Telegram/SourceFiles/tfc/data/tf_content_hash.cpp
Telegram/SourceFiles/tfc/data/tf_database.cpp
Telegram/SourceFiles/tfc/tf_infra.cpp
Telegram/SourceFiles/tfc/tf_settings.cpp
Telegram/SourceFiles/tfc/tf_worker.cpp
Telegram/SourceFiles/tfc/features/spy/online_tracker.cpp
Telegram/SourceFiles/tfc/features/spy/online_history_storage.cpp
Telegram/SourceFiles/tfc/features/teleforge/teleforge_bootstrap.cpp
Telegram/SourceFiles/tfc/features/teleforge/teleforge_storage.cpp
Telegram/SourceFiles/tfc/features/teleforge/tf_peer_archive.cpp
Telegram/SourceFiles/tfc/features/sync/teleforge_sync_merger.cpp
Telegram/SourceFiles/tfc/features/sync/teleforge_sync_snapshot.cpp
"

for src in $SOURCES; do
	[ -f "/usr/src/teleforge/$src" ] || { echo "=== $src === SKIP (no file)"; continue; }
	obj="${src#Telegram/SourceFiles/}.o"
	cmd=$(python3 - "$obj" <<'PY'
import re, sys
obj = sys.argv[1]
txt = open("build.ninja", encoding="utf-8", errors="ignore").read()
for block in txt.split("\nbuild "):
	if block.startswith("Telegram/CMakeFiles/Telegram.dir/SourceFiles/" + obj):
		d = re.search(r"DEFINES = (.*)", block)
		i = re.search(r"INCLUDES = (.*)", block)
		f = re.search(r"FLAGS = (.*)", block)
		if d and i and f:
			print("%s %s %s" % (d.group(1), i.group(1), f.group(1)))
		break
PY
)
	if [ -z "$cmd" ]; then
		echo "=== $src === SKIP (no target)"
		continue
	fi
	echo "=== $src ==="
	if g++ $cmd -fsyntax-only "/usr/src/teleforge/$src" 2>&1 | tail -5; then
		echo OK
	else
		echo FAIL
	fi
done
echo DONE
