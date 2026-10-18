#!/bin/bash
# Прогоняет проверки, которым нужен собранный бинарь.
# Запускать в WSL, где лежит сборочное дерево ~/tf.
set -u

TF_DIR="$HOME/tf"
BIN="$TF_DIR/out/bin/TeleForge"
WORK="$HOME/tfwork-check"

say() { echo; echo "=== $* ==="; }

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
	before=$(sqlite3 "$WORK/tdata/data.tforge" "select value from SchemaVersion" 2>/dev/null || echo "?")
	echo "схема до: $before"
	run_app
	after=$(sqlite3 "$WORK/tdata/data.tforge" "select value from SchemaVersion" 2>/dev/null || echo "?")
	echo "схема после: $after"
	echo "таблица GhostReadMessage: $(sqlite3 "$WORK/tdata/data.tforge" "select count(*) from sqlite_master where name='GhostReadMessage'" 2>/dev/null)"
	echo "индекс idx_ghost_read_peer: $(sqlite3 "$WORK/tdata/data.tforge" "select count(*) from sqlite_master where name='idx_ghost_read_peer'" 2>/dev/null)"
	echo "колонка accountId в PeerArchiveProfile: $(sqlite3 "$WORK/tdata/data.tforge" "select count(*) from pragma_table_info('PeerArchiveProfile') where name='accountId'" 2>/dev/null)"
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
