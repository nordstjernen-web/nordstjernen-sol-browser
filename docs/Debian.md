# Nordstjernen on Debian — packaging and distribution

This document records how Nordstjernen is packaged as a Debian `.deb`,
and — importantly — where in the Debian ecosystem it can actually live.
The packaging sources are in the `debian/` directory at the repository
root. For the plain build-from-source instructions (any distro) see
`Linux.md`; this document is specifically about the Debian package.

## Read this first: licensing

Nordstjernen is **dual-licensed**: each recipient may take it under
**either** the **Nordstjernen Source License v1.0 (NSL-1.0)** **or** the
**GNU General Public License version 3 or later (GPL-3.0-or-later)**, at
their option (SPDX `LicenseRef-NSL-1.0 OR GPL-3.0-or-later`; the texts are
`License.md` and `COPYING` at the repository root).

Debian `main` only accepts software whose license satisfies the [Debian
Free Software Guidelines
(DFSG)](https://www.debian.org/social_contract#guidelines). NSL-1.0 on
its own does not: its **"Competing Use"** restriction and its limit of
education and research use to **non-commercial** contexts both violate
DFSG §6 ("No Discrimination Against Fields of Endeavor"). The GPL,
however, is DFSG-free, and a dual license needs only one DFSG-free
option: Debian distributes the package under the GPL and recipients
keep the choice. The bundled third-party code (see
`THIRD-PARTY-LICENSES.md`) keeps its own free, GPL-3-compatible licenses.
**Nordstjernen can therefore go into Debian `main`**, through the usual
sponsorship and FTP-master review.

There are two paths, in order of effort:

1. **A third-party APT repository (fastest, fully under our control).**
   We host signed `.deb`s and an `apt` repo; users add it and
   `apt install nordstjernen-browser`. No Debian FTP-master review, works on
   Debian and every Debian derivative (Ubuntu, Mint, …). This is the
   practical channel until the package is in the archive, and is
   documented below.
2. **Debian `main`.** The GPL option qualifies the package for
   `main`. It still needs a Debian Developer to sponsor and upload it, an
   ITP bug, and FTP-master review of `debian/copyright`. The `debian/`
   tree here targets `main` (`Section: web`). Once in Debian, derivatives
   such as Ubuntu pick it up through their normal syncs (into Ubuntu's
   `universe`).

> Neither `non-free` nor `contrib` is the right home. `non-free` is for
> software with no DFSG-free license option, and `contrib` for DFSG-free
> software that *depends on* something outside `main`. Nordstjernen is
> DFSG-free under the GPL option and its build and runtime
> dependencies are in `main`.

The rest of this document covers building the `.deb` and shipping it
through paths (1) and (2).

## The `debian/` packaging tree

```
debian/
├── changelog          # version history; top entry sets the package version
├── control            # Section: web (main), build-deps, binary package
├── copyright          # DEP-5; GPL-3+ or NSL-1.0, plus bundled-code licenses
├── rules              # dh sequencer; feature switches, reproducible date
├── nordstjernen-browser.postinst   # registers the open-browser-nordstjernen alternative
├── nordstjernen-browser.prerm      # removes it
├── source/format      # 3.0 (quilt)
├── source/include-binaries   # the vendored wrap tarballs (debian/vendor/)
├── upstream/metadata  # DEP-12: repository, bug tracker, security contact
└── watch              # tracks upstream tags through the GitHub API
```

Key choices:

* **Package name `nordstjernen-browser`.** Source and binary package are
  both called `nordstjernen-browser`, so the archive name says what it is;
  the installed commands stay `nordstjernen`, `nordstjernen-renderer` and
  `nordstjernen-audio`.

* **`Section: web`** — the package targets `main`, distributed under the
  GPL option of the dual license.
* **`debian/rules`** is a small `dh` file. Hardening is on via
  `DEB_BUILD_MAINT_OPTIONS = hardening=+all,-fortify`; dpkg's fortify
  flag is left out because the meson build already sets
  `-D_FORTIFY_SOURCE=3` (with PIE and the stack protector), which dpkg's
  `=2` would override.
  The `dh_auto_configure` override passes `-Dwebgpu=disabled`
  (wgpu-native is not packaged; web-text-lib, the text-shaping Pango fork,
  is vendored at `src/web-text-lib/` and builds offline in sbuild/pbuilder
  chroots straight from the tarball), `-Davif=enabled -Djxl=enabled
  -Dgstreamer=enabled -Daudio=enabled` (so a missing build dependency
  fails the build instead of silently dropping a codec) and `-Dbuild_date`
  derived from `SOURCE_DATE_EPOCH`, so the About dialog's date comes from
  `debian/changelog` and the build is reproducible.
  An `execute_after_dh_auto_install` hook drops `libnordstjernen.so` and its
  header: that is the embedding API for other applications, nothing in the
  package links it, and it carries no SONAME version. The same hook
  removes the installed `open-browser-nordstjernen` link, which
  `nordstjernen-browser.postinst` registers through `update-alternatives`
  instead.
* **`debian/copyright`** is DEP-5. `Files: *` declares
  `License: GPL-3+ or NSL-1.0`, with a `GPL-3+` stanza pointing at
  `/usr/share/common-licenses/GPL-3` and an `NSL-1.0` stanza for the
  alternative. It also spells out the free licenses of the in-tree
  engine and the fetched ones (QuickJS — Expat; WAMR — Apache-2.0 with
  LLVM exception), the single-file libraries (Wuffs —
  Apache-2.0; pl_mpeg — Expat; minimp3 — CC0-1.0), the wgpu-native
  headers (BSD-3-Clause; Expat or Apache-2.0) and the CLDR data tables
  in `src/quickjs-bellard/intl/` (Unicode-3.0). `Files-Excluded` drops the
  Android port's prebuilt `gradle-wrapper.jar` when `uscan` repacks the
  upstream tarball.

## Build dependencies

The Debian build needs the `Build-Depends` of `debian/control` plus the
packaging tools. From the checkout, `apt-get build-dep` reads them
directly:

    sudo apt install build-essential debhelper devscripts
    sudo apt build-dep ./

The version floors in `debian/control` mirror the ones `meson setup`
enforces: GTK ≥ 4.14, libcurl ≥ 8.11, FFmpeg ≥ 6.0 (`7:6.0` with Debian's
epoch) and GStreamer ≥ 1.20, so Debian 13 (trixie) and Ubuntu 25.04 are
the oldest releases the package builds on.

The FFmpeg `libav*` packages are **not** optional here: `meson setup` fails
without them on Linux (inline WebM — VP9/VP8 video, Opus/Vorbis audio).
GStreamer decodes H.264/AAC, `libbrotli-dev` enables WOFF2 web fonts,
`libjxl-dev`/`libavif-dev` add JPEG XL and AVIF images. `libfontconfig-dev` and
`libfreetype-dev`, with GTK's cairo, HarfBuzz and FriBiDi development
packages, are what the in-tree web-text-lib text stack builds against.
`libsimde-dev` supplies the header-only SIMDe library for WebAssembly
SIMD, so meson does not fetch `subprojects/simde.wrap`, and `glslang-dev`
is the shader validator WebGL is built with.

`libseccomp-dev` is marked `[linux-any]` in `debian/control`; all Debian
release architectures are Linux, so it always applies. `cmake` is **not**
a build dependency: QuickJS and web-text-lib build in-tree via meson
`subdir()`, Wuffs and WAMR build as meson subprojects from their wrap
tarballs, and there are no CMake subprojects. `rustc` (1.100 or
newer) **is** one: meson compiles the in-tree Rust HTML and URL parsers
(`src/html5_rs/`, `src/url_rs/`) with plain `rustc`, without Cargo or any
crates.

Runtime dependencies are computed automatically by `dh_shlibdeps`
(`${shlibs:Depends}`) from the linked shared libraries — nothing is
hand-listed or bundled. The GStreamer plugin packages
(`gstreamer1.0-plugins-base`/`-good`/`-bad`, `gstreamer1.0-libav`) are
`Recommends`: they hold the H.264/AAC parsers and decoders that
`decodebin` picks at runtime. `ca-certificates` and a hunspell dictionary
are `Recommends` too.

## Downloaded sources in an offline build

Meson downloads three pinned upstream tarballs through wraps: PDF.js
(`subprojects/pdfjs-dist.wrap`), the Wuffs image decoders
(`subprojects/wuffs.wrap`) and the WAMR WebAssembly runtime
(`subprojects/wamr.wrap`, patched from `subprojects/packagefiles/`).
sbuild and pbuilder have no network, so fetch the tarballs into
`debian/vendor/` before making the source package:

    scripts/fetch-wrap-sources.sh

The script checks each tarball against the SHA-256 in its wrap file.
`debian/source/include-binaries` lets the source package carry them, and
`debian/rules` copies them into `subprojects/packagecache/` before meson
runs. Wuffs is required. Without PDF.js the package still builds, and a
PDF then shows a notice with a download link instead of the viewer. `debian/vendor/` is not
committed.

For Debian `main` this is not enough: the tarball holds minified
script, and Debian wants the corresponding source built in the archive.

## Build the package

From a checkout that contains the `debian/` directory:

    dpkg-buildpackage -us -uc -b

`-b` builds a binary-only package; drop it for a full source+binary
build. The result lands in the parent directory:

    ../nordstjernen-browser_<version>_<arch>.deb

Install and smoke-test it:

    sudo apt install ../nordstjernen-browser_*.deb
    nordstjernen

### Clean, reproducible builds with sbuild/pbuilder

Archive-quality builds happen in a minimal chroot so build-dependency
mistakes surface immediately. With `sbuild`:

    sudo sbuild-createchroot --include=eatmydata,ccache \
        unstable /srv/chroot/unstable-amd64 http://deb.debian.org/debian
    sbuild -d unstable

or with `pbuilder`:

    sudo pbuilder create
    pdebuild

### Lint before publishing

    lintian -i -I --show-overrides ../nordstjernen-browser_*.changes

For `main` the package should be lintian-clean. Any `license-problem-*`
tag — on Nordstjernen's own sources or on a **bundled** engine — is a
real issue to fix before upload.

## Path 1 — host a third-party APT repository

This is the route that gets Nordstjernen onto users' Debian/Ubuntu
machines without Debian's archive process.

1. **Build the `.deb`** as above (build once per architecture: `amd64`,
   `arm64`).

2. **Create a signed repository.** [`aptly`](https://www.aptly.info) is
   the simplest tool:

       aptly repo create -distribution=stable -component=main nordstjernen
       aptly repo add nordstjernen ../nordstjernen-browser_*.deb
       aptly publish repo -gpg-key=<KEYID> nordstjernen

   Serve `~/.aptly/public` over HTTPS (e.g. at
   `https://apt.nordstjernen.org`).

3. **Users add the repo** with a keyring (the modern, `signed-by`
   form — never `apt-key`):

       curl -fsSL https://apt.nordstjernen.org/nordstjernen.gpg \
         | sudo tee /usr/share/keyrings/nordstjernen.gpg >/dev/null
       echo "deb [signed-by=/usr/share/keyrings/nordstjernen.gpg] \
         https://apt.nordstjernen.org stable main" \
         | sudo tee /etc/apt/sources.list.d/nordstjernen.list
       sudo apt update && sudo apt install nordstjernen-browser

Updates ship by adding the new `.deb` to the repo and re-publishing;
`apt upgrade` then picks it up. Ship a `.deb` per release tag so
`debian/changelog`'s top version matches the upstream version.

## Path 2 — Debian `main`

With a Debian Developer willing to sponsor it (where the review stands is
under [Status of the Debian submission](#status-of-the-debian-submission)
below):

1. **File an ITP** (Intent To Package) bug against `wnpp`:

       reportbug --email <you> wnpp

   Title it `ITP: nordstjernen-browser -- web browser` and
   give the license as `GPL-3+ or NSL-1.0` (SPDX
   `LicenseRef-NSL-1.0 OR GPL-3.0-or-later`), noting that Debian distributes
   it under the GPL-3+ option and that it targets `main`.

2. **Polish the source package.** Build cleanly in `sbuild`, get
   `lintian` quiet, and make sure `debian/copyright` is complete and
   accurate. FTP-masters review this closely: the dual license and the
   bundled-engine licenses must all be listed.

3. **Note the embedded code copies.** Debian discourages bundled library
   copies (Debian Policy §4.13). Nordstjernen *forks* QuickJS in-tree
   (`src/quickjs-bellard/`) and builds WAMR from its release tarball with
   a local patch (`subprojects/packagefiles/wamr-nordstjernen.patch`),
   both modified for tight integration, so they
   cannot simply be swapped for system packages. Expect the sponsor and
   FTP-masters to ask about this: explain the reasoning, keep the
   `debian/copyright` accounting exhaustive, and expect the forks to be
   recorded in the security tracker's embedded-code-copies list.

4. **Upload via the sponsor.** A DD signs and uploads to the archive;
   after NEW-queue review the package enters `unstable` in `main`,
   migrates to `testing`, and is mirrored by Debian.

`debian/watch` lets `uscan` track upstream GitHub tags (through the
GitHub API, which the tags HTML page no longer serves reliably) so the
sponsor can spot new releases.

### Status of the Debian submission

The package is reviewed through sponsorship on
[mentors.debian.net](https://mentors.debian.net/package/nordstjernen/).
Update this section after each upload and each review.

| Upload | Version | Date | Result |
|---|---|---|---|
| #1 | `nordstjernen 1.0.35+ds-1` | 2026-10-20 | Needs work (two reviews, 2026-10-21); deleted from mentors by the uploader |
| #2 | `nordstjernen-browser 1.0.36+ds-1` | 2026-10-23 | Uploaded; source lintian clean, binary build verified in `debian:unstable` |

The two reviews of upload #1 asked for an ITP closed from a single
`Initial release` changelog entry, `execute_after_dh_auto_install`
instead of an override that re-ran `dh_auto_install`, plain `rm` instead
of `rm -f`, no `Rules-Requires-Root: no`, and debhelper compat 14; one
suggested keeping the packaging in a separate salsa.debian.org repository.
Upload #2 carries all of those except the ITP and the salsa move, and
renamed the source and binary package from `nordstjernen` to
`nordstjernen-browser`.

Still to do before the next upload:

- File the ITP bug against `wnpp` (step 1 above) and close it from the
  changelog: `* Initial release (Closes: #NNNNNNN)`, distribution
  `unstable`.
- Collapse `debian/changelog` to that one entry for the upload. In the
  tree it has one `UNRELEASED` entry per upstream release since, which
  the third-party repository path uses.
- Resolve the downloaded sources: the PDF.js tarball holds minified
  script (see "Downloaded sources in an offline build"), so build with
  `-Dpdfjs=disabled` or package its source.
- File the RFS (Request For Sponsorship) bug against
  `sponsorship-requests` once the upload is clean.
- Decide whether to move the packaging to a separate salsa.debian.org
  repository (gbp branches `upstream/latest`, `pristine-tar`,
  `debian/latest`, plus `debian/salsa-ci.yml`) and point `Vcs-*` there.
  Recommended by the first reviewer, not required.
- `debian-watch-does-not-check-openpgp-signature` (lintian X): GitHub
  tarballs are unsigned; either sign release tags or state in
  `debian/watch` that there is no signature to check.
- `no-manual-page` (lintian W, seen in a local binary build): ship man
  pages for `nordstjernen`, `nordstjernen-renderer` and
  `nordstjernen-audio`.
- `very-long-line-length-in-source-file` (lintian X, vendored and
  generated files): ignore or add `debian/source/lintian-overrides`.

## Updating the package

For both paths the per-release flow is:

1. Tag the upstream release and update `version:` in `meson.build`.
2. Add a top entry to `debian/changelog`
   (`dch -v <version>-1 "New upstream release"`); set the distribution
   (`unstable` for Debian, or your repo's codename).
3. Rebuild with `dpkg-buildpackage`/`sbuild`.
4. For Path 1, add the new `.deb` to the `aptly` repo and re-publish; for
   Path 2, hand the new source package to the sponsor.

## Troubleshooting

* **`dpkg-shlibdeps: warning: ... not found`** — a runtime library's
  `-dev` package is missing from `Build-Depends`; add it and rebuild in a
  clean chroot to catch the rest.
* **`lintian: license-problem-*`** on an engine under `src/` or
  `subprojects/` — a bundled component's license is misdeclared; fix
  `debian/copyright` to match `THIRD-PARTY-LICENSES.md`.
* **Sandbox warnings at runtime** (`seccomp: load failed`) — harmless
  outside a confined environment; the per-tab Landlock + seccomp sandbox
  logs and continues if the host policy blocks it. See `security-model.md`.
