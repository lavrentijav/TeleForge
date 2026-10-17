#!/usr/bin/env bash
# Publish one of the TeleForge PKGBUILDs to the AUR.
#
# Run from an Arch Linux environment (or the archlinux container, see README).
# The AUR account must have your SSH key registered, see packaging/aur/README.md.
#
# Usage:
#   publish.sh <teleforge-bin|teleforge-git> [aur-username]
#
# The first push of a package that does not exist yet creates it on the AUR.
set -euo pipefail

PKG="${1:?package name: teleforge-bin or teleforge-git}"
AUR_USER="${2:-${AUR_USER:-}}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_PKGBUILD="$HERE/$PKG/PKGBUILD"
WORK="${AUR_WORKDIR:-$HOME/aur-work}/$PKG"

if [[ ! -f "$SRC_PKGBUILD" ]]; then
	echo "No PKGBUILD at $SRC_PKGBUILD" >&2
	exit 1
fi

if [[ -z "$AUR_USER" ]]; then
	AUR_USER="${USER:-$(id -un)}"
fi

echo "[aur] package: $PKG  user: $AUR_USER"
export GIT_SSH_COMMAND="ssh -o StrictHostKeyChecking=accept-new"

if [[ ! -d "$WORK/.git" ]]; then
	echo "[aur] cloning existing AUR repository"
	mkdir -p "$(dirname "$WORK")"
	if ! git clone "ssh://aur@aur.archlinux.org/$PKG.git" "$WORK" 2>/dev/null; then
		echo "[aur] package does not exist yet, creating it locally"
		mkdir -p "$WORK"
		git -C "$WORK" init -q
		git -C "$WORK" remote add aur "ssh://aur@aur.archlinux.org/$PKG.git"
	fi
fi

cd "$WORK"
git config user.name "$AUR_USER"
git config user.email "$AUR_USER@users.noreply.github.com"

cp "$SRC_PKGBUILD" PKGBUILD

if command -v updpkgsums >/dev/null 2>&1; then
	updpkgsums
fi

makepkg --printsrcinfo > .SRCINFO

git add PKGBUILD .SRCINFO
if git diff --cached --quiet; then
	echo "[aur] nothing changed, leaving the AUR package as is"
	exit 0
fi

VERSION="$(sed -n 's/^pkgver=//p' PKGBUILD)-$(sed -n 's/^pkgrel=//p' PKGBUILD)"
git commit -q -m "upgpkg: $PKG $VERSION"

echo "[aur] pushing to AUR"
git push aur master 2>/dev/null || git push aur HEAD:master

echo "[aur] done: https://aur.archlinux.org/packages/$PKG"
