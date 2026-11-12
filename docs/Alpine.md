# Alpine Linux packaging

`data/packaging/APKBUILD` builds Nordstjernen from source on Alpine
(musl) with `abuild`. It produces a minimal package: the `nordstjernen`,
`nordstjernen-renderer` and `nordstjernen-audio`
binaries plus icons, the desktop file, the
i18n catalogues and the license texts — **nothing is bundled**. Every runtime
library (GTK 4, libcurl, OpenSSL, libwebp, libavif, FFmpeg, …)
is resolved from Alpine system packages, and `abuild`'s `tracedeps` derives
the `depends=` automatically from the linked shared objects, so the
dependency list is always exactly what the binary needs.

The build is fully offline: Wuffs, pl_mpeg and web-text-lib (the
text-shaping Pango fork, `src/web-text-lib/`) are vendored in the source
tarball and built from it, so nothing is fetched during Alpine's
network-isolated `build()` phase. `build()` passes `-Dwebgpu=disabled`:
wgpu-native publishes no musl release. The APKBUILD
declares `arch="x86_64 aarch64"`.

`makedepends` also lists every auto-detected optional library, so the
package gets the full feature set: libjxl (JPEG XL), GStreamer with
gst-plugins-base (H.264/AAC through the OS decoder), enchant2
(spell-checking), libvorbis/opusfile (native Ogg audio), brotli (WOFF2)
and freetype. GStreamer still needs a decoder at runtime to play H.264 —
`apk add gst-libav` or `gst-plugins-bad` — which the package does not
pull in.

This is separate from the nightly Alpine `.apk`, which
`scripts/pack-apk.sh` produces by repackaging the `scripts/pack-linux.sh`
bundle. Its APKBUILD has `!tracedeps`, so the script derives `depends=` by
asking `apk info -W` which package owns each library the staged binaries
load (see `Nightly.md`).

GitHub names the archive root after the repository, so the tarball unpacks
to `nordstjernen-sol-browser-$pkgver` rather than `$pkgname-$pkgver`; the
APKBUILD sets `builddir` accordingly.

## License

Nordstjernen is **dual-licensed**: each recipient may take it under
**either** the **Nordstjernen Source License v1.0** (`License.md`) **or**
the **GNU General Public License version 3 or later** (`COPYING`), at their
option. The APKBUILD declares this as
`license="LicenseRef-NSL-1.0 OR GPL-3.0-or-later"` and installs both texts to
`/usr/share/licenses/$pkgname/`.

NSL-1.0 on its own is a Functional-Source-License-style
*source-available* license with a "Competing Use" restriction and is not
OSI-approved. The GPL is, so the package is free software under that
option, and the bundled third-party code (see `THIRD-PARTY-LICENSES.md`)
keeps its own GPL-3-compatible licenses. Alpine's official `aports`
(main/community/testing) require free / OSI-approved licenses; the
GPL option meets that, so Nordstjernen can be proposed for `aports`
through the normal merge-request review (see below). Until it is
accepted, the same APKBUILD builds for a **personal / custom Alpine
repository**.

## Build and install locally (custom repo)

```sh
# On Alpine, as a user in the abuild group:
sudo apk add alpine-sdk
abuild-keygen -a -i

# From a checkout of this repo:
cd data/packaging
abuild -r              # fetches the pkgver source tarball, builds, packages

# Install the result:
sudo apk add --allow-untrusted \
    ~/packages/*/$(uname -m)/nordstjernen-<pkgver>-r0.apk
```

`abuild -r` installs `makedepends`, downloads the source during the
network-allowed fetch phase, then builds offline.

## Updating the version

Bump `pkgver` in `data/packaging/APKBUILD` to the new release tag, reset
`pkgrel=0`, then refresh the checksum:

```sh
cd data/packaging
abuild checksum        # rewrites sha512sums from the fetched tarball
```

The `source=` URL points at the GitHub release tarball
`.../archive/refs/tags/$pkgver.tar.gz`. Note that GitHub's
auto-generated archive checksums are not guaranteed stable forever; if a
checksum mismatch appears, re-run `abuild checksum`.

## Submitting to official aports

To propose it for `aports`:

1. Fork <https://gitlab.alpinelinux.org/alpine/aports>.
2. Add the APKBUILD under `testing/nordstjernen/APKBUILD` (new packages
   start in `testing/`, then move to `community/` after review).
3. Keep `license="LicenseRef-NSL-1.0 OR GPL-3.0-or-later"` and the
   `package()` lines that install `License.md` and `COPYING` under
   `/usr/share/licenses/$pkgname/`, and say in the merge request that the
   package is distributed under the GPL option.
4. Verify it builds in a clean chroot with `abuild rootbld` and passes
   `apkbuild-lint` / `apkbuild-shellcheck`.
5. Commit with the message `testing/nordstjernen: new aport` and open a
   merge request against `aports`.

See <https://wiki.alpinelinux.org/wiki/Creating_an_Alpine_package> for
the full contributor workflow.
