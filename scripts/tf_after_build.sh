#!/bin/bash
# Прогоняет проверки, которым нужен собранный бинарь.
# Запускать в WSL, где лежит сборочное дерево ~/tf.
set -u

TF_DIR="$HOME/tf"
BIN="$TF_DIR/out/bin/TeleForge"
WORK="$HOME/tfwork-check"

say() { echo; echo "=== $* ==="; }
# sqlite3 в WSL часто нет, поэтому читаем базу через python3
q() {
	python3 - "$1" "$2" <<'PY'
import sqlite3, sys
path, query = sys.argv[1], sys.argv[2]
try:
	print(sqlite3.connect(path).execute(query).fetchone()[0])
except Exception as e:
	print("?")
PY
}

if [ ! -x "$BIN" ]; then
	echo "нет бинаря: $BIN"
	exit 1
fi

# бинарь должен быть новее всех объектников, иначе проверяем старый код
STALE=$(find "$TF_DIR/out/Telegram/CMakeFiles" -name "*.o" -newer "$BIN" 2>/dev/null | wc -l)
if [ "$STALE" -ne 0 ]; then
	echo "бинарь устарел: $STALE объектников новее него"
	echo "сначала завершите сборку"
	exit 2
fi

say "бинарь"
ls -l "$BIN" | awk '{print $5, $6, $7, $8, $9}'
file "$BIN" | cut -c1-90

prepare() {
	rm -rf "$WORK"
	mkdir -p "$WORK/tdata"
}

run_app() {
	# 45 секунд достаточно, чтобы инициализация и миграция завершились
	timeout 45 "$BIN" -workdir "$WORK" -noupdate >/dev/null 2>&1
	true
}

say "сценарий 1: миграция data.tforge версии 14 -> 16"
prepare
if [ -f "$HOME/tfwork/tdata/data.tforge" ]; then
	cp "$HOME/tfwork/tdata/data.tforge" "$WORK/tdata/data.tforge"
	before=$(q "$WORK/tdata/data.tforge" "select version from SchemaVersion where singletonId = 1")
	echo "схема до: $before"
	run_app
	after=$(q "$WORK/tdata/data.tforge" "select version from SchemaVersion where singletonId = 1")
	echo "схема после: $after"
	echo "таблица GhostReadMessage: $(q "$WORK/tdata/data.tforge" "select count(*) from sqlite_master where name='GhostReadMessage'")"
	echo "индекс idx_ghost_read_peer: $(q "$WORK/tdata/data.tforge" "select count(*) from sqlite_master where name='idx_ghost_read_peer'")"
	echo "колонка accountId в PeerArchiveProfile: $(q "$WORK/tdata/data.tforge" "select count(*) from pragma_table_info('PeerArchiveProfile') where name='accountId'")"
else
	echo "нет копии БД $HOME/tfwork/tdata/data.tforge — сценарий пропущен"
fi

say "сценарий 2: импорт legacy ayudata.db"
prepare
if [ -f "$HOME/tfwork/tdata/data.tforge" ]; then
	cp "$HOME/tfwork/tdata/data.tforge" "$WORK/tdata/ayudata.db"
	run_app
	echo "файлы в tdata:"
	ls "$WORK/tdata" | grep -E "tforge|migrated|ayudata" | sed 's/^/  /'
	echo "лог про legacy:"
	grep -iE "legacy|import" "$WORK/log.txt" 2>/dev/null | tail -3 | sed 's/^/  /'
else
	echo "нет копии БД — сценарий пропущен"
fi

say "итог"
echo "рабочая папка проверок: $WORK"
