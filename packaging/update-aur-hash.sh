#!/bin/bash
# Обновляет sha256sums_x86_64 в PKGBUILD по актуальному архиву из dist/.
set -euo pipefail

PKG="${1:-$(dirname "$0")/aur/teleforge-bin/PKGBUILD}"
TAR="${2:-}"

if [ -z "$TAR" ]; then
	echo "usage: $0 [PKGBUILD] <archive.tar.xz>"
	exit 1
fi
if [ ! -f "$PKG" ]; then
	echo "PKGBUILD not found: $PKG"
	exit 1
fi
if [ ! -f "$TAR" ]; then
	echo "archive not found: $TAR"
	exit 1
fi

NEW=$(sha256sum "$TAR" | cut -d' ' -f1)
OLD=$(grep -oP "sha256sums_x86_64=\('\K[0-9a-f]+" "$PKG" || true)

echo "old: ${OLD:-<none>}"
echo "new: $NEW"

if [ "${OLD:-}" = "$NEW" ]; then
	echo "already up to date"
	exit 0
fi

sed -i "s/sha256sums_x86_64=('.*')/sha256sums_x86_64=('$NEW')/" "$PKG"
grep -n 'sha256sums_x86_64' "$PKG"
echo "updated"
