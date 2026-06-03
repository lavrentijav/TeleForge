#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
BUILD_DIR="${ROOT}/out/Release"
APPIMAGE_TOOL="${APPIMAGE_TOOL:-linuxdeployqt}"

cmake --build "${ROOT}/out" --config Release --target Telegram

if ! command -v "${APPIMAGE_TOOL}" >/dev/null 2>&1; then
  echo "linuxdeployqt not found. Install linuxdeploy or set APPIMAGE_TOOL."
  exit 1
fi

"${APPIMAGE_TOOL}" "${BUILD_DIR}/TeleForge" \
  -appimage \
  -bundle-non-qt-libs \
  -extra-plugins=iconengines,imageformats \
  -verbose=2

echo "AppImage build finished in ${BUILD_DIR}"
