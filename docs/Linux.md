# Nordstjernen on Linux — build, run, package

This document records the working setup for building and packaging
Nordstjernen on Linux. The primary supported targets are Debian /
Ubuntu, Fedora / RHEL, and openSUSE on `x86_64`. Local Linux is the
correctness gate: every commit must pass `meson compile -C builddir`
locally before pushing.

## Build dependencies

On Debian / Ubuntu, install the full build dependency list from
`debian/control` (run in the source tree):

    sudo apt build-dep .

On Fedora / RHEL:

    sudo dnf install gcc pkgconf meson ninja-build gtk4-devel libepoxy-devel libcurl-devel \
        openssl-devel uchardet-devel libpsl-devel sqlite-devel libseccomp-devel \
        libwebp-devel SDL2-devel

On openSUSE:

    sudo zypper install gcc pkgconf meson ninja gtk4-devel libepoxy-devel libcurl-devel \
        libopenssl-devel libuchardet-devel libpsl-devel sqlite3-devel libseccomp-devel \
        libwebp-devel sdl2-compat-devel

GStreamer is required on Linux too (H.264/AAC, and VP9/AV1/HEVC in hardware):
`libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev` on Debian/Ubuntu,
`gstreamer1-devel gstreamer1-plugins-base-devel` on Fedora,
`gstreamer-devel gstreamer-plugins-base-devel` on openSUSE. At runtime install
`gstreamer-plugins-bad` and the GPU's VA-API driver (`intel-media-driver` on
Intel) for hardware decoding. Without SDL2 the build has no audio; meson warns.

On Arch Linux (packaging and the AUR: `Arch.md`):

    sudo pacman -S --needed base-devel meson gtk4 libepoxy curl uchardet \
        libpsl sqlite libseccomp libwebp sdl2 ffmpeg

On Alpine (musl libc):

    sudo apk add build-base linux-headers pkgconf meson ninja gtk4.0-dev \
        libepoxy-dev curl-dev openssl-dev uchardet-dev libpsl-dev sqlite-dev \
        libseccomp-dev libwebp-dev sdl2-dev

Alpine builds against musl rather than glibc, so the resulting binary
is not interchangeable with the glibc portable zip — run a musl build
on a musl system. Add `clang cmake git zip` if you are packaging with
the nightly scripts.

**Required** on Linux, for inline **WebM** playback (VP9/VP8 video +
Opus/Vorbis audio): the FFmpeg `libav*` development packages, version 6.0 or
newer. `meson setup` fails without them — YouTube and most modern sites serve
VP9/WebM, so the external-player fallback is not acceptable here.

    sudo apt install libavformat-dev libavcodec-dev libavutil-dev \
        libswscale-dev libswresample-dev                       # Debian / Ubuntu
    sudo dnf install ffmpeg-devel                               # Fedora / RHEL (RPM Fusion)
    sudo zypper install ffmpeg-9-libavformat-devel ffmpeg-9-libavcodec-devel \
        ffmpeg-9-libavutil-devel ffmpeg-9-libswscale-devel \
        ffmpeg-9-libswresample-devel                            # openSUSE Tumbleweed (OSS)
    sudo apk add ffmpeg-dev                                     # Alpine

Optional, auto-detected: `libpoppler-glib-dev` (PDF rendering),
`libenchant-2-dev` plus a dictionary such as `hunspell-en-us`
(spell-checking), `libavif-dev` (AVIF; `-Davif=disabled` drops it) and
`libjxl-dev` (JPEG XL; `-Djxl=disabled` drops it).

`meson setup` checks version floors up front: libcurl 8.5+, GTK 4.14+,
and — for the default web-text-lib text stack — GLib 2.80+, cairo 1.18+,
HarfBuzz 8.3+, FriBiDi 1.0.6+ and fontconfig 2.15+. Ubuntu 24.04, Debian 13 and Fedora
40 are the oldest releases that ship all of them;
`-Dweb-text-lib=disabled` drops the text-stack floors by shaping through the
system Pango.

web-text-lib, the Pango fork that shapes text, is vendored in-tree at
`src/web-text-lib/` and builds with no network. On glibc
`x86_64`/`aarch64` the first `meson setup` does fetch the pinned
wgpu-native release for WebGPU (`subprojects/wgpu-native-linux-*.wrap`)
unless a system `wgpu_native` pkg-config file or `-Dwgpu_native_root`
supplies it. For an offline build pass `-Dwebgpu=disabled`, as the distro
packages do.

`ccache` is the biggest build-time win — `meson` picks it up
automatically. With ccache warm, a clean `meson setup builddir &&
meson compile -C builddir` drops from ~35 s to ~1 s. Install once
with the distro package manager.

### Optional: Rust IPC parser (`-Dipc_rust=enabled`)

The browser shell parses every message the sandboxed renderer sends it
(HTTP/1.1 heads, bodies and small JSON payloads, `src/ipc_http.c`). A
compromised renderer controls those bytes, so that parser is the sandbox's
attack surface. `-Dipc_rust=enabled` replaces it with a memory-safe Rust port
(`src/ipc_http_rs/lib.rs`, standard library only, no crates) behind the same
C ABI; the default build keeps the C parser. It needs `rustc` and GNU
binutils (`ld`, `objcopy`, `ar`), which `scripts/rust-staticlib-localize.sh`
uses to hide the Rust standard library inside the archive so it cannot clash
with wgpu-native's. The C shims pass the size of the shared structs, and the
Rust side refuses to parse if they differ from its own layout.
about:nordstjernen lists the active parser under Security.

    meson setup -Dipc_rust=enabled builddir-rust

With rustup but no default toolchain, point meson at one:
`RUSTC=~/.rustup/toolchains/<toolchain>/bin/rustc meson setup ...`.

## Develop

    meson setup builddir
    meson compile -C builddir
    ./builddir/src/gtk/nordstjernen

`./scripts/dev.sh build` runs `meson setup` (only if needed) and
`meson compile -C builddir` in one shot.

## Package — portable zip

`./scripts/pack-linux.sh` produces a redistributable, stripped,
LTO-optimised build for the host architecture (it configures a separate
release tree in `release-build/`, or `$BUILDDIR`):

    dist/nordstjernen-<version>-linux-<arch>.zip       # e.g. linux-x86_64
    dist/nordstjernen-<version>-linux-<arch>/          # unpacked bundle

The zip contains the `nordstjernen` shell, the sandboxed
`nordstjernen-renderer`, the `nordstjernen-audio` helper (when SDL2 was
found), the application icons, the desktop entry, `README.md`,
`THIRD-PARTY-LICENSES.md`, `License.md`, `COPYING`, and a generated
`INSTALL.md` listing the runtime requirements. When WebGPU is built,
`libwgpu_native.so` ships beside the binaries, found through an `$ORIGIN`
rpath (`NS_WEBGPU=0` packages without it).

The in-tree browser engine — lexbor, quickjs, wuffs and wamr — is
statically linked. The GTK desktop stack stays dynamic because it
expects to find pixbuf loaders, IM modules, and font/theme data on the
host at runtime; fully-static GTK isn't practical. Runtime requirements:

- glibc at least as new as the build host's (`INSTALL.md` records the
  floor read from the built binary)
- GTK 4.14+ with gio, gobject, pango, cairo
- libepoxy (usually pulled in by GTK 4; WebGL dispatch)
- libcurl 8.5+ with a TLS backend; OpenSSL 3 (libcrypto)
- libuchardet, libwebp, libpsl, libseccomp, libsqlite3, SDL2
- FFmpeg 6.0+ runtime libraries (libavformat, libavcodec, libavutil,
  libswscale, libswresample)
- libpoppler-glib and libavif, when the build found them
- fontconfig + a font set, harfbuzz, freetype, libstdc++
- ca-certificates (TLS trust store)
- An X11 or Wayland session

Smoke test the bundled binary headlessly without installing:

    ./dist/nordstjernen-<version>-linux-x86_64/nordstjernen \
        --headless --url=https://example.com --dump=text

## Package — RPM

`./scripts/pack-rpm.sh` repackages the same staged bundle as a
binary RPM. It calls `pack-linux.sh` first if the bundle is missing,
then drives `rpmbuild` against a generated spec under
`dist/rpmbuild/SPECS/`. The resulting RPM lands in `dist/`.

    sudo zypper install rpm-build       # openSUSE
    sudo dnf install rpm-build          # Fedora / RHEL
    sudo apt install rpm                # Debian / Ubuntu (ships rpmbuild)
    ./scripts/pack-rpm.sh

Output:

    dist/nordstjernen-<version>-1.x86_64.rpm

The spec uses `AutoReqProv: yes` so `rpmbuild` extracts the actual
SONAME dependencies (`libgtk-4.so.1`, `libcurl.so.4`,
`libuchardet.so.0`, the GLib stack, etc.) directly
from the binary's ELF dynamic section. The same RPM file therefore
installs on Fedora, RHEL, and openSUSE without per-distro tweaks —
each distro's resolver maps the SONAMEs to its own provider
packages. (Cross-installing into Debian / Ubuntu uses `alien`.)

Install layout (`%{_docdir}` is `/usr/share/doc/packages` on openSUSE,
`/usr/share/doc` on Fedora / RHEL):

    /usr/bin/nordstjernen
    /usr/bin/nordstjernen-renderer
    /usr/bin/nordstjernen-audio                      # when built
    /usr/lib64/nordstjernen/libwgpu_native.so        # when WebGPU is built
    /usr/share/icons/hicolor/scalable/apps/nordstjernen*
    /usr/share/applications/org.nordstjernen.WebBrowser.desktop
    /usr/share/nordstjernen/{License.md,COPYING}
    %{_docdir}/nordstjernen/{README.md,THIRD-PARTY-LICENSES.md}

The desktop file is installed under the application id so Wayland
compositors can match the window (`StartupWMClass`) to it.

Inspect, install, remove:

    rpm -qpi dist/nordstjernen-<version>-1.x86_64.rpm   # metadata
    rpm -qpR dist/nordstjernen-<version>-1.x86_64.rpm   # required SONAMEs
    sudo dnf install ./dist/nordstjernen-<version>-1.x86_64.rpm
    sudo rpm -e nordstjernen

The `%post` / `%postun` scriptlets refresh `gtk-update-icon-cache`
and `update-desktop-database` if those tools are available, so the
application picker picks up the new entry without a re-login.

## Notes

The lower-bound glibc version pinned to `libc.so.6(GLIBC_2.x)` in
auto-generated requires reflects whatever the build host ships. If
you want broader portability than the build host's libc allows,
build inside the oldest base container that still meets the meson
version floors (see *Build dependencies*) and re-run `pack-linux.sh` +
`pack-rpm.sh` from there.

Two more packaging scripts follow the same pattern:
`./scripts/pack-deb.sh` repackages the `pack-linux.sh` bundle as a
`.deb` (`dist/nordstjernen_<version>_[<distro-tag>_]<arch>.deb`; see
`Debian.md` for the source package in `debian/`), and
`./scripts/pack-appimage.sh` builds
`dist/nordstjernen-<version>-<arch>.AppImage` with linuxdeploy and its
GTK plugin, bundling the GTK 4 stack for distros without a modern GTK.
