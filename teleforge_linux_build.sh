#!/usr/bin/env bash
# TeleForge Linux build (x86_64 / aarch64) via official tdesktop Docker toolchain.
set -euo pipefail

usage() {
	cat <<'EOF'
Usage: teleforge_linux_build.sh [x86_64|aarch64] [Release|Debug]

Builds TeleForge for Linux inside the centos_env Docker image.
Requires: docker (QEMU/binfmt for aarch64 on x86_64 hosts).

Examples:
  ./teleforge_linux_build.sh x86_64
  ./teleforge_linux_build.sh aarch64 Release

Output:
  out-linux-<arch>/<Config>/bin/TeleForge
EOF
}

ARCH="${1:-x86_64}"
CONFIG="${2:-Release}"

case "$ARCH" in
	x86_64|x64|amd64)
		ARCH="x86_64"
		DOCKER_PLATFORM="linux/amd64"
		OUT_DIR="out-linux-x64"
		;;
	aarch64|arm64|arm)
		ARCH="aarch64"
		DOCKER_PLATFORM="linux/arm64"
		OUT_DIR="out-linux-aarch64"
		;;
	-h|--help|help)
		usage
		exit 0
		;;
	*)
		echo "Unknown arch: $ARCH" >&2
		usage
		exit 1
		;;
esac

TDESKTOP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ -d "${TDESKTOP_DIR}/../Libraries" ]]; then
	MOUNT_ROOT="$(cd "${TDESKTOP_DIR}/.." && pwd)"
else
	MOUNT_ROOT="$TDESKTOP_DIR"
fi

if [[ ! -d "$TDESKTOP_DIR/Telegram" ]]; then
	echo "Not found: $TDESKTOP_DIR/Telegram" >&2
	exit 1
fi

if ! command -v docker >/dev/null 2>&1; then
	echo "docker not found in PATH" >&2
	exit 1
fi

if ! docker info >/dev/null 2>&1; then
	echo "Docker daemon is not running. Start Docker Desktop and retry." >&2
	exit 1
fi

API_ID="${TDESKTOP_API_ID:-29721186}"
API_HASH="${TDESKTOP_API_HASH:-196a54e79899fbf76333ded98ff5027f}"
IMAGE="${TELEFORGE_LINUX_IMAGE:-ghcr.io/telegramdesktop/tdesktop/centos_env:latest}"

LLAMA_CMAKE=""
if [[ -d "${MOUNT_ROOT}/Libraries/win64/llama.cpp/CMakeLists.txt" ]]; then
	LLAMA_CMAKE="-D DESKTOP_APP_USE_LLAMA_CPP=ON -D LLAMA_CPP_ROOT=/usr/src/teleforge/Libraries/win64/llama.cpp"
	echo "[teleforge] llama.cpp: enabled"
else
	LLAMA_CMAKE="-D DESKTOP_APP_USE_LLAMA_CPP=OFF"
	echo "[teleforge] llama.cpp: disabled (Libraries/win64/llama.cpp not found)"
fi

SRC_REL="${TDESKTOP_DIR#$MOUNT_ROOT/}"

echo "[teleforge] arch=$ARCH platform=$DOCKER_PLATFORM config=$CONFIG"
echo "[teleforge] image=$IMAGE"
echo "[teleforge] mount=$MOUNT_ROOT"
echo "[teleforge] source=$SRC_REL"

docker pull --platform "$DOCKER_PLATFORM" "$IMAGE"

docker run --rm \
	--platform "$DOCKER_PLATFORM" \
	-u "$(id -u)" \
	-v "${MOUNT_ROOT}:/usr/src/teleforge" \
	-e CONFIG="$CONFIG" \
	-e OUT_DIR="$OUT_DIR" \
	-e API_ID="$API_ID" \
	-e API_HASH="$API_HASH" \
	-e LLAMA_CMAKE="$LLAMA_CMAKE" \
	-e SRC_REL="$SRC_REL" \
	"$IMAGE" \
	bash -lc '
		set -e
		cd "/usr/src/teleforge/${SRC_REL}/Telegram"
		rm -rf ../out
		# shellcheck disable=SC2086
		./configure.sh \
			-D TDESKTOP_API_ID=${API_ID} \
			-D TDESKTOP_API_HASH=${API_HASH} \
			${LLAMA_CMAKE} \
			-D DESKTOP_APP_DISABLE_AUTOUPDATE=ON \
			-D DESKTOP_APP_DISABLE_CRASH_REPORTS=ON
		cmake --build ../out --config "${CONFIG}" --target Telegram --parallel "$(nproc)"
		rm -rf "../${OUT_DIR}"
		mv ../out "../${OUT_DIR}"
	'

BIN="${TDESKTOP_DIR}/${OUT_DIR}/${CONFIG}/bin/TeleForge"
if [[ ! -f "$BIN" ]]; then
	BIN="${TDESKTOP_DIR}/${OUT_DIR}/${CONFIG}/TeleForge"
fi

if [[ ! -f "$BIN" ]]; then
	echo "[teleforge] Build finished but binary not found under ${OUT_DIR}/${CONFIG}" >&2
	exit 1
fi

file "$BIN"
ls -lh "$BIN"
echo "[teleforge] Done: $BIN"
