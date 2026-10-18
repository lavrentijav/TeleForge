#!/usr/bin/env bash
# Build .tar.xz, .deb and (when rpmbuild is available) .rpm artifacts from a
# staged `cmake --install` tree.
#
# Usage: package.sh <staging-root> <version> <arch> <output-dir>
#
# The staging root is a DESTDIR tree: it contains usr/bin/..., usr/share/... .
# The script refuses to package an incomplete tree and verifies every artifact
# it produces, so an empty (control-only) package can never be published.
set -euo pipefail

STAGE="$(cd "${1:?staging root}" && pwd)"
VERSION="${2:?version}"
ARCH="${3:?arch}"
OUT="${4:?output dir}"
PKGNAME="teleforge"

# One arch name per ecosystem: the tarball uses the uname spelling, Debian and
# RPM have their own.
case "$ARCH" in
x86_64 | amd64)
	TAR_ARCH="x86_64"
	DEB_ARCH="amd64"
	RPM_ARCH="x86_64"
	;;
aarch64 | arm64)
	TAR_ARCH="aarch64"
	DEB_ARCH="arm64"
	RPM_ARCH="aarch64"
	;;
*)
	TAR_ARCH="$ARCH"
	DEB_ARCH="$ARCH"
	RPM_ARCH="$ARCH"
	;;
esac

NAME="TeleForge-${VERSION}-${TAR_ARCH}"
TARBALL="$OUT/${NAME}.tar.xz"
DEB="$OUT/${PKGNAME}_${VERSION}_${DEB_ARCH}.deb"

mkdir -p "$OUT"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# --- staging tree checks -----------------------------------------------------
# Without these the script used to package whatever happened to be in the tree:
# an empty or half-installed staging root produced a .deb of a few hundred bytes
# (DEBIAN/ only, no data member) that still looked like a successful build.
if [[ ! -d "$STAGE/usr" ]]; then
	echo "staging root has no usr/ directory: $STAGE" >&2
	exit 1
fi
if [[ ! -f "$STAGE/usr/bin/TeleForge" ]]; then
	echo "staging root has no usr/bin/TeleForge: $STAGE" >&2
	echo "the install tree is incomplete - refusing to build empty packages" >&2
	exit 1
fi
if [[ ! -s "$STAGE/usr/bin/TeleForge" ]]; then
	echo "staging root has an empty usr/bin/TeleForge: $STAGE" >&2
	exit 1
fi

stage_files=$(find "$STAGE/usr" -type f | wc -l)
stage_bytes=$(du -sb "$STAGE/usr" | cut -f1)
echo "==> staging: $stage_files files, $stage_bytes bytes in $STAGE"

# The build bakes CMAKE_INSTALL_FULL_BINDIR into the D-Bus service, so a tree
# configured with the default /usr/local prefix points at a path these packages
# never install into and D-Bus activation silently fails.
fix_dbus_service() {
	local root="$1"
	local svc="$root/usr/share/dbus-1/services/com.teleforge.desktop.service"
	[[ -f "$svc" ]] || return 0

	local dir file_mtime dir_mtime
	dir="$(dirname "$svc")"
	file_mtime="$(stat -c %y "$svc")"
	dir_mtime="$(stat -c %y "$dir")"

	# Rewrite in place rather than with `sed -i`: a temp file in the payload
	# directory would bump both timestamps and make the tarball hash change on
	# every run.
	local rewritten
	rewritten="$(mktemp)"
	sed 's|^Exec=.*|Exec=/usr/bin/TeleForge|' "$svc" > "$rewritten"
	cat "$rewritten" > "$svc"
	rm -f "$rewritten"
	touch -d "$file_mtime" "$svc"
	touch -d "$dir_mtime" "$dir"
}

# Copy the payload and prove it arrived, so a truncated copy fails loudly here
# instead of turning into an empty package further down.
copy_payload() {
	local dest="$1"
	mkdir -p "$dest"
	cp -a "$STAGE/usr/." "$dest/usr/"
	fix_dbus_service "$dest"

	local copied
	copied=$(find "$dest/usr" -type f | wc -l)
	if [[ "$copied" -ne "$stage_files" ]]; then
		echo "payload copy into $dest is incomplete: $copied of $stage_files files" >&2
		exit 1
	fi
	if [[ ! -s "$dest/usr/bin/TeleForge" ]]; then
		echo "payload copy into $dest lost usr/bin/TeleForge" >&2
		exit 1
	fi
}

rm -f "$TARBALL" "$DEB"

echo "==> tar.xz"
mkdir -p "$WORK/tar"
copy_payload "$WORK/tar"
tar -C "$WORK/tar" --sort=name --owner=0 --group=0 --numeric-owner \
	-cJf "$TARBALL" usr
tar -tJf "$TARBALL" > "$WORK/tar-list.txt"
if ! grep -qx 'usr/bin/TeleForge' "$WORK/tar-list.txt"; then
	echo "built tarball has no usr/bin/TeleForge: $TARBALL" >&2
	exit 1
fi

echo "==> deb"
DEB_ROOT="$WORK/deb"
mkdir -p "$DEB_ROOT/DEBIAN"
copy_payload "$DEB_ROOT"

installed_size=$(du -sk "$DEB_ROOT/usr" | cut -f1)
# Generated from the binary's own DT_NEEDED set; Qt and libstdc++ are linked
# statically, so they are not listed. The t64 alternates keep the package
# installable on both bookworm and trixie: the C++ runtime is static, but GLib,
# Pango, cairo, fontconfig and libpng are not.
cat > "$DEB_ROOT/DEBIAN/control" <<EOF
Package: $PKGNAME
Version: $VERSION
Section: net
Priority: optional
Architecture: $DEB_ARCH
Maintainer: TeleForge <lavrentijav@gmail.com>
Installed-Size: $installed_size
Depends: libatomic1, libblkid1, libbrotli1, libbz2-1.0, libc6, libcairo2, libdatrie1, libexpat1, libffi8, libfontconfig1, libfreetype6, libfribidi0, libglib2.0-0t64 | libglib2.0-0, libgraphite2-3, libharfbuzz0b, libmount1, libpango-1.0-0, libpangocairo-1.0-0, libpangoft2-1.0-0, libpcre2-8-0, libpixman-1-0, libpng16-16t64 | libpng16-16, libselinux1, libthai0, libx11-6, libxau6, libxcb-render0, libxcb-shm0, libxcb1, libxdmcp6, libxext6, libxrender1, zlib1g
Recommends: xdg-desktop-portal, desktop-file-utils, xdg-utils
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

# Stamp the generated control members with a timestamp taken from the payload:
# dpkg-deb records the mtimes it finds, so leaving them at build time makes the
# same staging tree produce a different .deb (and hash) on every run.
ref_mtime="$(stat -c %y "$STAGE/usr/bin/TeleForge")"
touch -d "$ref_mtime" "$DEB_ROOT/DEBIAN/control" "$DEB_ROOT/DEBIAN/postinst" \
	"$DEB_ROOT/DEBIAN/postrm" "$DEB_ROOT/DEBIAN" "$DEB_ROOT"

dpkg-deb --root-owner-group --build "$DEB_ROOT" "$DEB"
dpkg-deb -c "$DEB" > "$WORK/deb-list.txt"
if ! grep -q '\./usr/bin/TeleForge$' "$WORK/deb-list.txt"; then
	echo "built .deb carries no payload, only control files: $DEB" >&2
	rm -f "$DEB"
	exit 1
fi

echo "==> rpm"
if command -v rpmbuild >/dev/null 2>&1; then
	RPM_ROOT="$WORK/rpm"
	mkdir -p "$RPM_ROOT/BUILD" "$RPM_ROOT/RPMS" "$RPM_ROOT/SOURCES" \
		"$RPM_ROOT/SPECS" "$RPM_ROOT/SRPMS" "$RPM_ROOT/BUILDROOT"

	# %files has to list what the staging tree really contains; the old hardcoded
	# /usr/bin/teleforge did not exist and broke the build.
	rpm_files=$(cd "$STAGE" && find usr -mindepth 1 \( -type f -o -type l \) | sort |
		sed 's|^|/|')

	cat > "$RPM_ROOT/SPECS/$PKGNAME.spec" <<EOF
Name: $PKGNAME
Version: $VERSION
Release: 1
Summary: TeleForge Desktop messenger
License: GPL-3.0-or-later
URL: https://github.com/lavrentijav/TeleForge
BuildArch: $RPM_ARCH
AutoReqProv: no
%global __os_install_post %{nil}
%global _build_id_links none

%description
Telegram Desktop based messenger with TeleForge extensions.

%install
mkdir -p %{buildroot}/usr
cp -a "$STAGE/usr/." %{buildroot}/usr/
sed -i 's|^Exec=.*|Exec=/usr/bin/TeleForge|' \
	%{buildroot}/usr/share/dbus-1/services/com.teleforge.desktop.service

%files
$rpm_files

%changelog
EOF

	if ! rpmbuild --define "_topdir $RPM_ROOT" -bb "$RPM_ROOT/SPECS/$PKGNAME.spec" \
		> "$WORK/rpmbuild.log" 2>&1; then
		echo "rpmbuild failed, log follows:" >&2
		tail -30 "$WORK/rpmbuild.log" >&2
		exit 1
	fi
	find "$RPM_ROOT/RPMS" -name '*.rpm' -exec cp -a {} "$OUT/" \;
else
	echo "rpmbuild not found - skipping .rpm (install the 'rpm' package to build it)" >&2
fi

echo "==> artifacts"
ls -lh "$OUT"
if command -v dpkg-deb >/dev/null 2>&1; then
	echo "==> ${DEB##*/}: $(dpkg-deb -c "$DEB" | wc -l) entries, control:"
	dpkg-deb -I "$DEB" | sed -n '/Package:/,$p'
fi
