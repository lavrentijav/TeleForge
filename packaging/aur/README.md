# AUR packages for TeleForge

Two packages, published independently:

| Package | Kind | Source of the payload |
| --- | --- | --- |
| `teleforge-bin` | prebuilt | `TeleForge-<version>-<arch>.tar.xz` from GitHub Releases, produced by `.github/workflows/teleforge_release.yml` |
| `teleforge-git` | from source | `git clone https://github.com/lavrentijav/TeleForge.git`, built with the system toolchain |

Both provide `teleforge` and `telegram-desktop`, so they conflict with each other and with the official
`telegram-desktop` packages.

## Publishing to the AUR

The AUR serves one git repository per package; a push to `ssh://aur@aur.archlinux.org/<pkgname>.git`
publishes it. Do this from an Arch box (or a container) with your AUR SSH key loaded.

```bash
# one-time, per package
git clone ssh://aur@aur.archlinux.org/teleforge-bin.git
git clone ssh://aur@aur.archlinux.org/teleforge-git.git

# every release, for the prebuilt package
cp packaging/aur/teleforge-bin/PKGBUILD teleforge-bin/
cd teleforge-bin
sed -i 's/^pkgver=.*/pkgver=6.9.0/' PKGBUILD # current release version
updpkgsums                                  # fills the real sha256sum of the tarball
makepkg --printsrcinfo > .SRCINFO
git add PKGBUILD .SRCINFO
git commit -m "upgpkg: $(sed -n 's/^pkgver=//p' PKGBUILD)-$(sed -n 's/^pkgrel=//p' PKGBUILD)"
git push
```

`teleforge-git` has no version to bump: `pkgver()` derives it from `Telegram/build/version` plus
the commit count, so `makepkg --printsrcinfo > .SRCINFO` followed by a push is enough after a
`PKGBUILD` change.

## Verifying a package locally

```bash
# build the package without installing
makepkg --syncdeps --cleanbuild --force

# inspect what it would install
makepkg --printsrcinfo > /dev/null && tar -tf teleforge-bin-*.pkg.tar.zst
```

`namcap teleforge-bin-*.pkg.tar.zst` reports missing or superfluous dependencies; the `depends`
array in the prebuilt package may need trimming once the exact shared-library set is known.

## Release assets the prebuilt package expects

`teleforge_release.yml` publishes, for both `x86_64` and `aarch64`:

- `TeleForge-<version>-<arch>.tar.xz` — a `DESTDIR` install tree (`usr/bin`, `usr/share`, ...)
- `teleforge_<version>_<debarch>.deb`
- `teleforge-<version>-1.<arch>.rpm`

If the tarball layout changes, `package()` in `teleforge-bin/PKGBUILD` has to follow it.
