# AUR packages for TeleForge

Two packages, published independently:

| Package | Kind | Source of the payload |
| --- | --- | --- |
| `teleforge-bin` | prebuilt | `TeleForge-<version>-<arch>.tar.xz` from GitHub Releases, produced by `.github/workflows/teleforge_release.yml` |
| `teleforge-git` | from source | `git clone https://github.com/lavrentijav/TeleForge.git`, built with the system toolchain |

Both provide `teleforge` and `telegram-desktop`, so they conflict with each other and with the official
`telegram-desktop` packages.

## One-time setup

1. Register an account at <https://aur.archlinux.org/register> (username, email,
   password). The username becomes part of your commit metadata.
2. Add an SSH key under **My Account → SSH Public Key**. Only one key per account,
   and it is used for every package you own.
3. Check the package names are free:
   `https://aur.archlinux.org/packages?K=teleforge` — `teleforge-bin` and
   `teleforge-git` are stored in separate AUR git repositories.
4. Verify the key works: `ssh aur@aur.archlinux.org help` must print
   `Welcome to AUR`. Accept the host key on first connect.

## Publishing to the AUR

The AUR serves one git repository per package; a push to `ssh://aur@aur.archlinux.org/<pkgname>.git`
publishes it, and the first push of a name that does not exist creates it. Branch is `master`.

`publish.sh` does the whole cycle: clone or init the AUR repository, copy the
`PKGBUILD`, refresh checksums, regenerate `.SRCINFO`, commit and push.

```bash
packaging/aur/publish.sh teleforge-bin <aur-username>
packaging/aur/publish.sh teleforge-git <aur-username>
```

From Windows, run it inside an Arch container with the key mounted:

```powershell
docker run --rm -it `
  -v "$HOME\.ssh:/root/.ssh:ro" `
  -v "${PWD}:/src" -w /src `
  archlinux:latest bash -c "pacman -Sy --noconfirm pacman-contrib git openssh && \
    bash packaging/aur/publish.sh teleforge-bin <aur-username>"
```

Manual equivalent:

```bash
git clone ssh://aur@aur.archlinux.org/teleforge-bin.git
cp packaging/aur/teleforge-bin/PKGBUILD teleforge-bin/
cd teleforge-bin
sed -i 's/^pkgver=.*/pkgver=6.9.0/' PKGBUILD # current release version
updpkgsums                                  # fills the real sha256sum of the tarball
makepkg --printsrcinfo > .SRCINFO
git add PKGBUILD .SRCINFO
git commit -m "upgpkg: $(sed -n 's/^pkgver=//p' PKGBUILD)-$(sed -n 's/^pkgrel=//p' PKGBUILD)"
git push aur master
```

`teleforge-git` has no version to bump: `pkgver()` derives it from `Telegram/build/version` plus
the commit count, so `makepkg --printsrcinfo > .SRCINFO` followed by a push is enough after a
`PKGBUILD` change.

If a package needs to be removed, use **Delete package** on the AUR web page; nobody else can take
the name until then.

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
