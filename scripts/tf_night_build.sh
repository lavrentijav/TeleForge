#!/bin/bash
# Запускает сборку TeleForge в полностью отсоединённом виде внутри WSL.
#
# Зачем: сборка идёт часами, а процессы, запущенные из агентской сессии или
# терминала, умирают вместе с ней. setsid отвязывает сборку от сессии, и она
# продолжается, даже если терминал закрыт.
#
# Использование:  bash scripts/tf_night_build.sh [JOBS]
# Логи:          ~/tf/night-build.log
# Маркер конца:  ~/tf/night-build.exit

set -u

JOBS="${1:-2}"
TF_DIR="$HOME/tf"
LOG="$TF_DIR/night-build.log"
EXIT_FILE="$TF_DIR/night-build.exit"

if docker ps -q | grep -q .; then
	echo "уже есть запущенные контейнеры:"
	docker ps --format '  {{.ID}} {{.Command}}'
	echo "остановите их (docker kill), чтобы не было двух сборок на одном out/"
	exit 1
fi

free_mb=$(free -m | awk '/^Mem:/ {print $7}')
if [ "$free_mb" -lt 1500 ]; then
	echo "мало памяти: доступно ${free_mb} МиБ, нужно хотя бы 1500"
	exit 1
fi

rm -f "$EXIT_FILE"
echo "запуск: JOBS=$JOBS, лог $LOG"

setsid bash -c "
	TF_IMAGE=teleforge/centos_env:full JOBS=$JOBS \
		bash '$TF_DIR/tf_build_wsl.sh' Release > '$LOG' 2>&1
	echo \"EXIT=\$?\" >> '$LOG'
	touch '$EXIT_FILE'
" < /dev/null > /dev/null 2>&1 &

sleep 5
echo "процесс отсоединён, следить за прогрессом:"
echo "  tail -f $LOG"
echo "  grep -c 'error:' $LOG"
echo "  ls $EXIT_FILE   # появится, когда сборка закончится"
