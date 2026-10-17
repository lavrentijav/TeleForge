#!/usr/bin/env bash
# TeleForge Scanner — automated install/update over SSH, driven by hosts.toml.
#
# Usage:
#   ./deploy.sh [hosts.toml]
#
# For every [[host]] entry: rsyncs the module, creates/updates a venv,
# installs requirements, uploads the per-host config (if given), and
# (optionally) installs + restarts the systemd unit. Idempotent — safe to
# rerun to push updates.
set -euo pipefail

HOSTS_FILE="${1:-$(dirname "$0")/hosts.toml}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MODULE_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

if [[ ! -f "$HOSTS_FILE" ]]; then
	echo "hosts file not found: $HOSTS_FILE" >&2
	echo "Copy deploy/hosts.example.toml to hosts.toml and edit it first." >&2
	exit 1
fi

if ! command -v python3 >/dev/null 2>&1; then
	echo "python3 is required to parse $HOSTS_FILE" >&2
	exit 1
fi

# Emit one "name\tssh\tport\tremote_dir\tconfig\tidentity\tenable_service" line
# per host so bash can loop without a TOML parser of its own.
mapfile -t HOST_LINES < <(python3 - "$HOSTS_FILE" <<'PY'
import sys
try:
	import tomllib
except ModuleNotFoundError:
	import tomli as tomllib

with open(sys.argv[1], "rb") as f:
	data = tomllib.load(f)

for h in data.get("host", []):
	print("\t".join([
		h.get("name", ""),
		h.get("ssh", ""),
		str(h.get("port", 22)),
		h.get("remote_dir", "/opt/teleforge-scanner"),
		h.get("config", ""),
		h.get("identity", ""),
		"1" if h.get("enable_service", True) else "0",
	]))
PY
)

if [[ ${#HOST_LINES[@]} -eq 0 ]]; then
	echo "No [[host]] entries in $HOSTS_FILE" >&2
	exit 1
fi

for line in "${HOST_LINES[@]}"; do
	IFS=$'\t' read -r NAME SSH_TARGET PORT REMOTE_DIR CONFIG IDENTITY ENABLE_SERVICE <<< "$line"

	if [[ -z "$SSH_TARGET" ]]; then
		echo "skipping host '$NAME': no ssh target" >&2
		continue
	fi

	SSH_OPTS=(-p "$PORT" -o BatchMode=yes -o StrictHostKeyChecking=accept-new)
	SCP_OPTS=(-P "$PORT" -o BatchMode=yes -o StrictHostKeyChecking=accept-new)
	if [[ -n "$IDENTITY" ]]; then
		SSH_OPTS+=(-i "$IDENTITY")
		SCP_OPTS+=(-i "$IDENTITY")
	fi

	echo "== $NAME ($SSH_TARGET:$PORT) -> $REMOTE_DIR =="

	ssh "${SSH_OPTS[@]}" "$SSH_TARGET" "mkdir -p '$REMOTE_DIR'"

	rsync -az --delete \
		-e "ssh ${SSH_OPTS[*]}" \
		--exclude '__pycache__' --exclude '*.pyc' --exclude '.venv' \
		--exclude 'media' --exclude '*.db' --exclude 'config.toml' \
		--exclude 'hosts.toml' --exclude '.git' \
		"$MODULE_DIR"/ "$SSH_TARGET:$REMOTE_DIR/"

	if [[ -n "$CONFIG" ]]; then
		if [[ ! -f "$CONFIG" ]]; then
			echo "  config '$CONFIG' not found locally, skipping upload" >&2
		else
			scp "${SCP_OPTS[@]}" "$CONFIG" "$SSH_TARGET:$REMOTE_DIR/config.toml"
		fi
	fi

	ssh "${SSH_OPTS[@]}" "$SSH_TARGET" bash -s <<REMOTE
set -euo pipefail
cd '$REMOTE_DIR'
if ! command -v python3 >/dev/null 2>&1; then
	echo "python3 missing on remote host" >&2
	exit 1
fi
python3 -m venv .venv --upgrade-deps >/dev/null 2>&1 || python3 -m venv .venv
./.venv/bin/pip install --quiet --upgrade pip
./.venv/bin/pip install --quiet -r requirements.txt
echo "  deps installed"
REMOTE

	if [[ "$ENABLE_SERVICE" == "1" ]]; then
		ssh "${SSH_OPTS[@]}" "$SSH_TARGET" bash -s <<REMOTE
set -euo pipefail
if [[ \$EUID -ne 0 ]] && ! sudo -n true 2>/dev/null; then
	echo "  no root/passwordless sudo on remote, skipping systemd install" >&2
	exit 0
fi
SUDO=""
[[ \$EUID -ne 0 ]] && SUDO="sudo"
\$SUDO cp '$REMOTE_DIR/deploy/teleforge-scanner.service' /etc/systemd/system/teleforge-scanner.service
\$SUDO sed -i "s#WorkingDirectory=.*#WorkingDirectory=$REMOTE_DIR#; s#ExecStart=.*#ExecStart=$REMOTE_DIR/.venv/bin/python -m teleforge_scanner --config $REMOTE_DIR/config.toml#" /etc/systemd/system/teleforge-scanner.service
\$SUDO systemctl daemon-reload
\$SUDO systemctl enable --now teleforge-scanner.service
\$SUDO systemctl restart teleforge-scanner.service
echo "  service (re)started"
REMOTE
	fi

	echo "== $NAME done =="
done
