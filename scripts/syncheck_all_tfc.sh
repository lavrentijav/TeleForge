#!/bin/bash
# Проверяет компиляцию всех .cpp из tfc/ с реальными флагами из build.ninja.
# Цели берутся напрямую из build.ninja, поэтому имена совпадают точно.
set +e

cd /usr/src/teleforge/out || exit 1

cat > /tmp/one_check.sh <<'INNER'
#!/bin/bash
obj="$1"
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
[ -z "$cmd" ] && { echo "SKIP $obj"; exit 0; }
src="/usr/src/teleforge/Telegram/SourceFiles/${obj%.o}"
out=$(g++ $cmd -fsyntax-only "$src" 2>&1 | grep -m2 "error:")
if [ -n "$out" ]; then
	echo "FAIL $obj"
	echo "$out" | sed 's/^/    /'
else
	echo "OK $obj"
fi
INNER
chmod +x /tmp/one_check.sh

grep -o "Telegram/CMakeFiles/Telegram.dir/SourceFiles/tfc/[^:]*\.cpp\.o" build.ninja | \
	sort -u | sed 's#Telegram/CMakeFiles/Telegram.dir/SourceFiles/##' | \
	xargs -P "${JOBS:-3}" -I{} /tmp/one_check.sh {} | tee /usr/src/teleforge/syncheck_all.log | \
	grep -E "^(OK|FAIL|SKIP) " | awk '{print $1}' | sort | uniq -c
echo "--- failures ---"
grep -A2 "^FAIL " /usr/src/teleforge/syncheck_all.log || echo "none"
