#!/bin/bash
# Проверки схемы и миграций TeleForge на копии рабочей БД.
# Использование: ./tf_db_checks.sh <путь-к-data.tforge> [бинарь]
set -euo pipefail

DB="${1:?usage: $0 <path-to-data.tforge> [binary]}"
BIN="${2:-$HOME/tf/out/bin/TeleForge}"
WORK=$(mktemp -d /tmp/tfdb.XXXXXX)
trap 'rm -rf "$WORK"' EXIT

mkdir -p "$WORK/tdata"
cp "$DB" "$WORK/tdata/data.tforge"

echo "=== before ==="
python3 - "$WORK/tdata/data.tforge" <<'PY'
import sqlite3, sys
db = sqlite3.connect(sys.argv[1])
cur = db.cursor()
ver = cur.execute("select version from SchemaVersion").fetchone()
tabs = {r[0] for r in cur.execute("select name from sqlite_master where type='table'")}
print("version:", ver[0] if ver else "none")
print("GhostReadMessage:", "GhostReadMessage" in tabs)
print("counts:", {t: cur.execute(f"select count(*) from {t}").fetchone()[0]
                  for t in ("DeletedMessage", "EditedMessage") if t in tabs})
db.close()
PY

echo "=== run ==="
cd "$WORK"
timeout 40 "$BIN" -workdir "$WORK" -noupdate > /dev/null 2>&1 || true

echo "=== log ==="
grep -iE "schema|migrat|ghost|archive" "$WORK/log.txt" 2>/dev/null | tail -12 || true

echo "=== after ==="
python3 - "$WORK/tdata/data.tforge" <<'PY'
import sqlite3, sys
db = sqlite3.connect(sys.argv[1])
cur = db.cursor()
ver = cur.execute("select version from SchemaVersion").fetchone()
tabs = {r[0] for r in cur.execute("select name from sqlite_master where type='table'")}
idx = {r[0] for r in cur.execute("select name from sqlite_master where type='index'")}
print("version:", ver[0] if ver else "none")
print("GhostReadMessage:", "GhostReadMessage" in tabs)
print("idx_ghost_read_peer:", "idx_ghost_read_peer" in idx)
cols = [c[1] for c in cur.execute("PRAGMA table_info(PeerArchiveProfile)")]
print("PeerArchiveProfile.accountId:", "accountId" in cols)
print("counts:", {t: cur.execute(f"select count(*) from {t}").fetchone()[0]
                  for t in ("DeletedMessage", "EditedMessage") if t in tabs})
db.close()
PY
