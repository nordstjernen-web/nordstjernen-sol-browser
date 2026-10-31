# Nordstjernen on openSUSE — packaging and distribution

This document records how Nordstjernen is packaged for openSUSE through
the [Open Build Service (OBS)](https://build.opensuse.org), where it can
live, and how the package is built from a release tarball. For plain
build-from-source instructions (any distro) see `Linux.md`; this document
is specifically about the openSUSE RPM and OBS.

The build recipe (`nordstjernen.spec`) and openSUSE changelog
(`nordstjernen.changes`) live at the repository root; OBS gets copies of
them next to the release tarball. Notes for maintainers, including the
release steps, are in `packaging/obs/README.md`.

## Install it (users)

Nordstjernen is built in the OBS home project
[`home:andreasrosdal`](https://build.opensuse.org/package/show/home:andreasrosdal/nordstjernen).
Add the repository and install; updates then arrive through `zypper`:

```sh
# openSUSE Tumbleweed
sudo zypper addrepo https://download.opensuse.org/repositories/home:/andreasrosdal/openSUSE_Tumbleweed/home:andreasrosdal.repo
sudo zypper refresh
sudo zypper install nordstjernen
```

For Leap, replace `openSUSE_Tumbleweed` with your release (e.g. `16.0`).
The package builds for `x86_64` and `aarch64`.

Prefer a one-off install without adding the repo? Grab the nightly RPM
directly: [`nordstjernen-opensuse-x86_64.rpm`](https://www.nordstjernen.org/nightly/nordstjernen-opensuse-x86_64.rpm)
(rebuilt from `main` each night; `sudo zypper install ./…rpm`).

## Read this first: licensing

Nordstjernen is **dual-licensed**: each recipient may take it under
**either** the **Nordstjernen Source License v1.0 (NSL-1.0)** **or** the
**GNU General Public License version 3 or later (GPL-3.0-or-later)**, at
their option. The spec declares this as `License: LicenseRef-NSL-1.0 OR GPL-3.0-or-later`.

openSUSE:Factory (Tumbleweed's main / OSS distribution) only accepts free
/ OSI-approved licenses, checked by its automated
[Cavil](https://github.com/openSUSE/cavil) legal scan and the legal team.
NSL-1.0 on its own would not pass — its **"Competing Use"** restriction
and its limit of education and research use to **non-commercial**
contexts make it non-free — but the GPL is free and OSI-approved, and
the package can be distributed under that option. The bundled third-party
code (see `THIRD-PARTY-LICENSES.md`) keeps its own free, GPL-3-compatible
licenses. **Nordstjernen is therefore eligible for Factory**, subject to
the usual review.

There are two paths, in order of effort:

1. **The OBS home project (fully under our control).** We build and
   publish RPMs; users add the repo and `zypper install`. No Factory
   review. This is what the install section above uses, and it stays
   available whatever happens in Factory.
2. **openSUSE:Factory (the OSS distribution).** Possible through the
   GPL option. The package goes into a devel project and from there,
   by submit request, to `openSUSE:Factory`, where the devel project
   maintainers, the legal team (Cavil) and the Factory reviewers check
   it. Expect the legal review to look at the `LicenseRef-NSL-1.0` half
   of the expression, since it is not an SPDX-listed license; the
   `GPL-3.0-or-later` alternative is what qualifies the package. The devel
   project is **`network`**: `home:andreasrosdal/nordstjernen` is submitted
   to `network/nordstjernen`, and from there forwarded to Factory.

The non-OSS component (`openSUSE:Factory:NonFree` → the `non-oss` repo) is
no longer the target: it is for software without a free license option.

## Release tarball package

Factory requires sources that trace to an upstream release, so the OBS
package is a classic one: `nordstjernen.spec`, `nordstjernen.changes`
and `nordstjernen-<version>.tar.gz`, the GitHub archive of the release
tag named by the spec's `Source0`:

```spec
Source0:        https://github.com/nordstjernen-web/nordstjernen-sol-browser/archive/refs/tags/%{version}.tar.gz#/%{name}-%{version}.tar.gz

%prep
%autosetup -n nordstjernen-sol-browser-%{version}
```

The build workers have no network access, so the tarball is uploaded to
OBS with `osc`, never fetched at build time, and never committed to git.

## Build options and dependencies

The spec configures meson with one feature off:

    -Dwebgpu=disabled   # needs external wgpu-native, not packaged

web-text-lib, the text-shaping Pango fork, is vendored at
`src/web-text-lib/` and builds from the tarball, so OBS workers need no
network for it.

The package is built for 64-bit targets only (`ExcludeArch: %ix86 %arm`):
the Temporal code in the in-tree QuickJS uses `__int128`, which GCC does
not offer on 32-bit x86 or ARM.

openSUSE's `%meson` passes `--auto-features=enabled`, so every `auto`
feature in `meson_options.txt` needs its dependency in `BuildRequires`;
a missing one fails `meson setup` with `Dependency "…" not found`.

Everything else builds from the declared `BuildRequires`:

    gcc gcc-c++ meson ninja pkgconfig hicolor-icon-theme
    pkgconfig(gtk4) pkgconfig(epoxy) pkgconfig(libcurl) pkgconfig(libcrypto)
    pkgconfig(uchardet) pkgconfig(libpsl) pkgconfig(sqlite3)
    pkgconfig(libwebp) pkgconfig(libavif) pkgconfig(libjxl) pkgconfig(sdl2)
    pkgconfig(gstreamer-1.0) pkgconfig(gstreamer-app-1.0)
    pkgconfig(gstreamer-video-1.0) pkgconfig(simde)
    pkgconfig(libavcodec) pkgconfig(libavformat) pkgconfig(libavutil)
    pkgconfig(libswresample) pkgconfig(libswscale)
    pkgconfig(fontconfig) pkgconfig(pango) pkgconfig(pangocairo)
    pkgconfig(pangoft2)
    pkgconfig(libseccomp) pkgconfig(enchant-2)

The FFmpeg `libav*` packages are required, not optional: meson fails without
them on Linux, and they carry inline WebM (VP9/VP8 + Opus/Vorbis). The Pango
packages are what GTK builds against; web-text-lib itself needs the cairo,
HarfBuzz, FriBiDi and fontconfig development files.

The in-tree engines (lexbor, QuickJS, WAMR, web-text-lib) and vendored single-file
libraries (Wuffs, pl_mpeg) build via meson `subdir()` / wraps — no `cmake`,
no system copies. `mpv` and `myspell-en_US` are `Recommends` (external
media playback and a spell-check dictionary).

The package is tagged `License: LicenseRef-NSL-1.0 OR GPL-3.0-or-later`;
`License.md` and `COPYING` (the GPL version 3 text) are shipped as `%license`.

## Local RPM build

Off OBS, you can build the same RPM from a checkout with `rpmbuild`, or use
the helper scripts `scripts/pack-rpm.sh` (portable repackage) and
`scripts/pack-srpm.sh` (source RPM). OBS rebuilds when a new spec,
changes file or tarball is committed with `osc ci`.

## Build the package set wider

Add more targets in the project's **Repositories** tab: openSUSE
Tumbleweed and Leap 16.0 are known good. Skip Debian targets (they expect
`debian/` packaging, not an RPM spec) and only keep the architectures you
care about.

## Troubleshooting

* **`Dependency "…" not found` at `meson setup`** — an `auto` feature
  gained a dependency that `BuildRequires` does not list; add it.
* **i586 or armv7 builds appear** — `ExcludeArch: %ix86 %arm` is missing
  from the spec.
* **Sandbox warnings at runtime** (`seccomp: load failed`) — harmless
  outside a confined environment; the per-tab Landlock + seccomp sandbox
  logs and continues. See `tab-isolation.md`.
