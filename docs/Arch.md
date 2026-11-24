# Nordstjernen on Arch Linux — packaging and the AUR

The Arch package recipe is `packaging/arch/PKGBUILD`, with its generated
`packaging/arch/.SRCINFO`. The package is named `nordstjernen-browser`, as in
Debian, so the name says what it is; the command it installs is still
`nordstjernen`. It builds a tagged release tarball with
`arch-meson`, the same way the Debian and openSUSE packages build it. For
plain build-from-source instructions see `Linux.md`.

## Build from source

```sh
sudo pacman -S --needed base-devel git rust meson gtk4 libepoxy curl uchardet \
    libpsl sqlite libseccomp libwebp sdl3 glslang ffmpeg gstreamer gst-plugins-base-libs \
    libavif libjxl enchant simde
git clone https://github.com/nordstjernen-web/nordstjernen-sol-browser.git
cd nordstjernen-sol-browser
meson setup builddir && meson compile -C builddir
./builddir/src/gtk/nordstjernen
```

## Build the package

```sh
cd packaging/arch
makepkg -si                 # build and install
extra-x86_64-build          # or: clean-chroot build (devtools), as Arch builds it
namcap PKGBUILD *.pkg.tar.zst
```

The package:

- passes `-Dwebgpu=disabled`. wgpu-native is not in the Arch repositories
  (only in the AUR), and meson would otherwise download a prebuilt release
  during `build()`.
- lists the pinned PDF.js, Wuffs and WAMR tarballs in `source=` and copies
  them into `subprojects/packagecache/` in `prepare()`, so meson does not
  download its wraps; SIMDe comes from the `simde` package.
- requires AVIF, JPEG XL, GStreamer and the audio helper explicitly, so a
  missing dependency fails the build instead of dropping a feature.
- drops `libnordstjernen.so` and its header (the embedding API, unversioned
  and unused by the browser).
- declares `license=('GPL-3.0-or-later OR LicenseRef-NSL-1.0')` and links
  `License.md` into `/usr/share/licenses/nordstjernen-browser/`, as Arch requires
  for a `LicenseRef-` license.

The one remaining `namcap` warning is `libgstvideo` linked but unused by
`nordstjernen-audio`, because the helper shares the GStreamer media sources
with the renderer. It does no harm.

## Submit it to the AUR

New packages enter Arch through the AUR; a package moves to the official
`extra` repository only when an Arch Package Maintainer adopts it. To
publish it, use an AUR account with an SSH key:

```sh
git clone ssh://aur@aur.archlinux.org/nordstjernen-browser.git aur-nordstjernen-browser
cp packaging/arch/PKGBUILD packaging/arch/.SRCINFO aur-nordstjernen-browser/
cd aur-nordstjernen-browser
git add PKGBUILD .SRCINFO
git commit -m "Initial upload: nordstjernen-browser 1.0.51"
git push
```

For each release, run `packaging/arch/update.sh <tag>` on Arch (it needs
`git`, `makepkg` and `pacman-contrib`). It sets `pkgver`, resets `pkgrel=1`,
sets the PDF.js, Wuffs and WAMR versions to what the tag pins in
`subprojects/*.wrap`, runs `updpkgsums`, checks that those three tarballs hash
as the wraps say, and regenerates `.SRCINFO`. Commit both files here and in
the AUR repository.

## CI

`.github/workflows/arch.yml` builds the package in an up-to-date
`archlinux:base-devel` container as a normal user, runs `namcap` (any error
fails the job), installs it and runs a headless smoke test. It runs on pull
requests that touch `packaging/arch/`, weekly (Arch is rolling, so a
dependency update can break an unchanged recipe), and on every pushed
version tag, where it first runs `update.sh` for that tag and uploads the
updated `PKGBUILD` and `.SRCINFO` as the `arch-package` artifact.

## Test VM

The package was verified on Arch Linux 2026.10.01 (linux 7.2, GCC 16,
FFmpeg 9, GTK 4.24) in a VirtualBox VM installed unattended from the
official ISO with a cloud-init seed. It built with no warnings in a clean
`extra-x86_64-build` chroot, and the browser ran in a GNOME Wayland session.
Release 1.0.36 was rebuilt with `makepkg -s` in a fresh
`archlinux:base-devel` podman container (GCC 16, FFmpeg 9, GTK 4.24) and
passes namcap with only the warning above.

## The other editions

Northstar and Southstar carry the same kind of recipe in their own
`packaging/arch/`, as `northstar-browser` and `southstar-browser`. All three
packages install side by side.
