#!/usr/bin/env bash
# Build .tar.xz, .deb and .rpm artifacts from a staged `cmake --install` tree.
#
# Usage: package.sh <staging-root> <version> <arch> <output-dir>
#
# The staging root is a DESTDIR tree: it contains usr/bin/..., usr/share/... .
set -euo pipefail

STAGE="$(cd "${1:?staging root}" && pwd)"
VERSION="${2:?version}"
ARCH="${3:?arch}"
OUT="${4:?output dir}"
NAME="TeleForge-${VERSION}-${ARCH}"
PKGNAME="teleforge"

mkdir -p "$OUT"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

if [[ ! -d "$STAGE/usr" ]]; then
	echo "staging root has no usr/ directory: $STAGE" >&2
	exit 1
fi

echo "==> tar.xz"
mkdir -p "$WORK/tar"
cp -a "$STAGE/usr" "$WORK/tar/usr"
tar -C "$WORK/tar" -cJf "$OUT/${NAME}.tar.xz" usr

echo "==> deb"
DEB_ROOT="$WORK/deb"
mkdir -p "$DEB_ROOT/DEBIAN"
cp -a "$STAGE/usr" "$DEB_ROOT/usr"

installed_size=$(du -sk "$DEB_ROOT/usr" | cut -f1)
cat > "$DEB_ROOT/DEBIAN/control" <<EOF
Package: $PKGNAME
Version: $VERSION
Section: net
Priority: optional
Architecture: $( [ "$ARCH" = "aarch64" ] && echo arm64 || echo amd64 )
Maintainer: TeleForge <lavrentijav@gmail.com>
Installed-Size: $installed_size
Depends: libc6, libstdc++6, libx11-6, libxcb1, zlib1g
Recommends: xdg-desktop-portal
Homepage: https://github.com/lavrentijav/TeleForge
Description: TeleForge Desktop messenger
 Telegram Desktop based messenger with TeleForge extensions.
EOF

cat > "$DEB_ROOT/DEBIAN/postinst" <<EOF
#!/bin/sh
set -e
if command -v update-desktop-database >/dev/null 2>&1; then
	update-desktop-database -q /usr/share/applications || true
fi
EOF
cat > "$DEB_ROOT/DEBIAN/postrm" <<EOF
#!/bin/sh
set -e
if command -v update-desktop-database >/dev/null 2>&1; then
	update-desktop-database -q /usr/share/applications || true
fi
EOF
chmod 0755 "$DEB_ROOT/DEBIAN/postinst" "$DEB_ROOT/DEBIAN/postrm"

dpkg-deb --root-owner-group --build "$DEB_ROOT" "$OUT/${PKGNAME}_${VERSION}_$( [ "$ARCH" = "aarch64" ] && echo arm64 || echo amd64 ).deb"

echo "==> rpm"
if command -v rpmbuild >/dev/null 2>&1; then
	RPM_ROOT="$WORK/rpm"
	mkdir -p "$RPM_ROOT/BUILD" "$RPM_ROOT/RPMS" "$RPM_ROOT/SOURCES" \
		"$RPM_ROOT/SPECS" "$RPM_ROOT/SRPMS" "$RPM_ROOT/BUILDROOT"

	cat > "$RPM_ROOT/SPECS/$PKGNAME.spec" <<EOF
Name: $PKGNAME
Version: $VERSION
Release: 1
Summary: TeleForge Desktop messenger
License: GPL-3.0-or-later
URL: https://github.com/lavrentijav/TeleForge
BuildArch: $ARCH
AutoReqProv: no
%global __os_install_post %{nil}

%description
Telegram Desktop based messenger with TeleForge extensions.

%install
mkdir -p %{buildroot}
cp -a $STAGE/usr %{buildroot}/usr

%files
/usr/bin/teleforge
/usr/share/applications/com.teleforge.desktop.desktop
/usr/share/metainfo/com.teleforge.desktop.metainfo.xml
/usr/share/icons/hicolor
/usr/share/dbus-1/services/com.teleforge.desktop.service

%changelog
EOF

	rpmbuild --define "_topdir $RPM_ROOT" -bb "$RPM_ROOT/SPECS/$PKGNAME.spec" >/dev/null
	find "$RPM_ROOT/RPMS" -name '*.rpm' -exec cp -a {} "$OUT/" \;
else
	echo "rpmbuild not found, skipping .rpm" >&2
fi

ls -lh "$OUT"
