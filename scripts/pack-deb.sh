#!/usr/bin/env bash
# Build a portable Nordstjernen .deb by repackaging the bundle that
# pack-linux.sh produces. The binary statically links the in-tree engine
# (lexbor, quickjs, wuffs). Stable desktop deps (GTK, curl, rsvg, …) are
# computed from the binary's SONAMEs with dpkg-shlibdeps, falling back to
# a hand-maintained list. The image-codec libraries whose SONAMEs differ
# per distro release (libavif and its AV1 codecs) are bundled in
# the package instead. The FFmpeg libav* dependencies keep their
# SONAME-derived package names (libavcodec60 on Ubuntu 24.04, libavcodec61
# on Debian 13, …) but have their version floor relaxed to the FFmpeg
# major.minor release (see relax_ffmpeg_floor), so a .deb built on a
# patched build host still installs on a system running an earlier patch
# release of the same ABI. Those package names tie the .deb to the distro
# release it was built on, so the file name carries that release
# (nordstjernen_<version>_ubuntu24.04_amd64.deb); DEB_DISTRO_TAG overrides
# the tag read from /etc/os-release, and an empty tag drops it.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
VERSION=${VERSION:-$(grep -E "^[[:space:]]*version" "$ROOT/meson.build" | head -1 \
          | sed -E "s/.*version: '([^']+)'.*/\1/")}
DEBARCH=$(dpkg --print-architecture 2>/dev/null || echo amd64)
if [ -z "${DEB_DISTRO_TAG+set}" ] && [ -r /etc/os-release ]; then
    DEB_DISTRO_TAG=$(. /etc/os-release \
        && printf '%s%s' "${ID:-}" "${VERSION_ID:-${VERSION_CODENAME:-}}")
fi
DISTRO_TAG=$(printf '%s' "${DEB_DISTRO_TAG:-}" | tr -cd 'A-Za-z0-9.')
ARCH=$(uname -m)
SLUG="nordstjernen-${VERSION}-linux-${ARCH}"
STAGE="$ROOT/dist/${SLUG}"

if ! command -v dpkg-deb >/dev/null 2>&1; then
    echo "dpkg-deb not found. Install it first:" >&2
    echo "    sudo apt install dpkg-dev            # Debian/Ubuntu" >&2
    exit 1
fi

if [ ! -x "$STAGE/nordstjernen" ]; then
    echo "Bundle not staged yet — running pack-linux.sh first."
    "$ROOT/scripts/pack-linux.sh"
fi

PKGROOT="$ROOT/dist/debpkg"
rm -rf "$PKGROOT"
install -dm755 "$PKGROOT/DEBIAN"
install -dm755 "$PKGROOT/usr/bin"
install -dm755 "$PKGROOT/usr/share/icons/hicolor/scalable/apps"
install -dm755 "$PKGROOT/usr/share/applications"
install -dm755 "$PKGROOT/usr/share/nordstjernen"
install -dm755 "$PKGROOT/usr/share/doc/nordstjernen"

install -m755 "$STAGE/nordstjernen" "$PKGROOT/usr/bin/nordstjernen"
install -m755 "$STAGE/nordstjernen-renderer" "$PKGROOT/usr/bin/nordstjernen-renderer"
# Audio playback helper, when SDL2 was available at build time. It is added to
# the dpkg-shlibdeps scan below so its libSDL2 dependency lands in Depends.
if [ -x "$STAGE/nordstjernen-audio" ]; then
    install -m755 "$STAGE/nordstjernen-audio" "$PKGROOT/usr/bin/nordstjernen-audio"
fi
# All app + toolbar icons the UI and about: pages look up by name.
for icon in "$ROOT"/data/icons/hicolor/scalable/apps/nordstjernen*.svg \
            "$ROOT"/data/icons/hicolor/scalable/apps/nordstjernen.gif; do
    [ -e "$icon" ] && install -m644 "$icon" \
        "$PKGROOT/usr/share/icons/hicolor/scalable/apps/"
done
# Desktop file named for the GTK app-id so Wayland matches the window to it
# (otherwise the taskbar/dock icon is blank).
install -m644 "$ROOT/data/nordstjernen.desktop" \
    "$PKGROOT/usr/share/applications/org.nordstjernen.WebBrowser.desktop"
# about:license and about:gpl read these at ../share/nordstjernen/ relative to
# the binary (/usr/bin -> /usr/share/nordstjernen).
install -m644 "$ROOT/License.md" "$PKGROOT/usr/share/nordstjernen/License.md"
install -m644 "$ROOT/COPYING" "$PKGROOT/usr/share/nordstjernen/COPYING"
install -m644 "$ROOT/README.md" "$PKGROOT/usr/share/doc/nordstjernen/"
install -m644 "$ROOT/THIRD-PARTY-LICENSES.md" "$PKGROOT/usr/share/doc/nordstjernen/"
install -m644 "$ROOT/License.md" "$PKGROOT/usr/share/doc/nordstjernen/copyright"

# Bundle the volatile image-codec libraries (libavif and the AV1
# codecs it pulls in) under /usr/lib/nordstjernen with an $ORIGIN rpath.
# Their SONAMEs bump between Ubuntu/Debian releases and each release ships
# only one version, so depending on them as system packages makes the .deb
# installable on exactly one release. The stable desktop libs (GTK, curl,
# sqlite, …) stay as normal dependencies.
# Best-effort section: media-lib bundling and shlibdeps both shell out to
# tools that can legitimately exit non-zero (dpkg -S on an unowned path,
# SIGPIPE from head, shlibdeps warnings). Disable errexit here so a hiccup
# degrades to system deps instead of aborting the whole package build.
set +e

BUNDLE_DIR="$PKGROOT/usr/lib/nordstjernen"
# Seed libs the binary links directly whose SONAMEs bump per distro release.
SEED_RE='libavif\.so'
# Never bundle the C/C++/OpenMP runtime: universally present and ABI-stable,
# and a newer system copy stays backward-compatible with our older build.
CORE_DENY='^(ld-linux|ld-musl|libc|libm|libmvec|libdl|libpthread|librt|libgcc_s|libstdc\+\+|libgomp|libnuma|libatomic)[.-]'
# Package-name families to drop from Depends once their libs are bundled.
# Matched against the dep name directly (not via dpkg -S, whose path lookup
# is unreliable under usrmerge), so it tracks whatever ldd linked.
STRIP_RE='^lib(avif|aom|dav1d|gav1|yuv|sharpyuv|rav1e|svtav1)'
# FFmpeg runtime packages. Debian/Ubuntu generate their shlibs floor with
# `dh_makeshlibs -V`, i.e. the exact upstream version of the build host's
# FFmpeg, so dpkg-shlibdeps emits e.g. `libavcodec61 (>= 7:7.1.5)` — which
# refuses to install on a machine still on 7:7.1.1 even though the SONAME
# (and hence the ABI the binary was linked against) is identical. FFmpeg
# patch releases are bugfix-only and never change the ABI, so the floor is
# relaxed to the major.minor release (`>= 7:7.1`); the SONAME-numbered
# package name keeps guarding the actual ABI.
FFMPEG_RE='^lib(avcodec|avformat|avutil|avfilter|avdevice|swscale|swresample|postproc)[0-9]+'
relax_ffmpeg_floor() {
    printf '%s' "$1" | sed -E 's/\(>= ([0-9]+:)?([0-9]+\.[0-9]+)[^)]*\)/(>= \1\2)/g'
}
bundled_any=0
if command -v patchelf >/dev/null 2>&1; then
    install -dm755 "$BUNDLE_DIR"
    # WebGPU: the engine links libwgpu_native.so directly (its DT_NEEDED was
    # rewritten to the bare soname by pack-linux.sh), and it is not a system
    # package, so a WebGPU-enabled build must carry the copy pack-linux staged
    # or the binaries won't even start. Bundle it like the codec libs below.
    if [ -e "$STAGE/libwgpu_native.so" ]; then
        cp -L "$STAGE/libwgpu_native.so" "$BUNDLE_DIR/libwgpu_native.so"
        chmod 644 "$BUNDLE_DIR/libwgpu_native.so"
        patchelf --set-rpath '$ORIGIN' "$BUNDLE_DIR/libwgpu_native.so" 2>/dev/null
    fi
    # BFS over the dependency closure of the seed codec libs, so every backend
    # libavif pulls in (gav1, aom, dav1d, rav1e, SvtAv1, yuv, sharpyuv, …)
    # is bundled too -- no per-name allow-list to keep in sync.
    worklist=$( { ldd "$PKGROOT/usr/bin/nordstjernen" 2>/dev/null; \
                  ldd "$PKGROOT/usr/bin/nordstjernen-renderer" 2>/dev/null; } \
                 | grep -oE '/[^ ]+\.so[^ ]*' | grep -E "$SEED_RE")
    seen=""
    while [ -n "$worklist" ]; do
        next=""
        for path in $worklist; do
            base=$(basename "$path")
            case " $seen " in *" $base "*) continue ;; esac
            seen="$seen $base"
            [ -e "$path" ] || continue
            if [ ! -e "$BUNDLE_DIR/$base" ]; then
                cp -L "$path" "$BUNDLE_DIR/$base" || continue
                chmod 644 "$BUNDLE_DIR/$base"
                patchelf --set-rpath '$ORIGIN' "$BUNDLE_DIR/$base" 2>/dev/null
            fi
            for dep in $(ldd "$path" 2>/dev/null | grep -oE '/[^ ]+\.so[^ ]*'); do
                db=$(basename "$dep")
                printf '%s' "$db" | grep -Eq "$CORE_DENY" && continue
                case " $seen " in *" $db "*) continue ;; esac
                next="$next $dep"
            done
        done
        worklist=$next
    done
    if [ -n "$(ls -A "$BUNDLE_DIR" 2>/dev/null)" ]; then
        rpath_ok=1
        # Both the launcher and the renderer link libavif (image decoding
        # lives in the engine both share), so both need the bundle on their
        # rpath -- otherwise the renderer fails to start with
        # "libavif.so.NN: cannot open shared object file".
        for bin in nordstjernen nordstjernen-renderer; do
            [ -e "$PKGROOT/usr/bin/$bin" ] || continue
            patchelf --set-rpath '$ORIGIN/../lib/nordstjernen' \
                "$PKGROOT/usr/bin/$bin" 2>/dev/null || rpath_ok=0
        done
        if [ "$rpath_ok" = 1 ]; then
            bundled_any=1
            echo "pack-deb: bundled$(ls "$BUNDLE_DIR" | sed 's/^/ /' | tr -d '\n')"
        else
            rm -rf "$BUNDLE_DIR"
        fi
    else
        rm -rf "$BUNDLE_DIR"
    fi
fi

INSTALLED_KB=$(du -sk "$PKGROOT/usr" | cut -f1)

FALLBACK_DEPS="libgtk-4-1, libepoxy0, libcurl4 | libcurl4t64, libuchardet0, libpsl5 | libpsl5t64, libsqlite3-0, libpoppler-glib8, libfontconfig1"

RUNTIME_DEPS=""
if command -v dpkg-shlibdeps >/dev/null 2>&1; then
    install -dm755 "$PKGROOT/debian"
    cat > "$PKGROOT/debian/control" <<CTL
Source: nordstjernen

Package: nordstjernen
Architecture: any
CTL
    # Scan the renderer too: it is the binary that actually decodes video, so
    # the FFmpeg libav* SONAMEs of an inline-WebM build land in Depends from
    # here (the GTK shell links them via the shared engine as well).
    scan_bins=(usr/bin/nordstjernen usr/bin/nordstjernen-renderer)
    [ -x "$PKGROOT/usr/bin/nordstjernen-audio" ] && scan_bins+=(usr/bin/nordstjernen-audio)
    RUNTIME_DEPS=$(cd "$PKGROOT" \
        && dpkg-shlibdeps -O --ignore-missing-info "${scan_bins[@]}" 2>/dev/null \
        | sed -n 's/^shlibs:Depends=//p')
    rm -rf "$PKGROOT/debian"
fi
[ -n "$RUNTIME_DEPS" ] || RUNTIME_DEPS="$FALLBACK_DEPS"

set -e

kept=""
OLDIFS=$IFS; IFS=','
for dep in $RUNTIME_DEPS; do
    d=$(printf '%s' "$dep" | sed -E 's/^[[:space:]]+//; s/[[:space:]]+$//')
    [ -n "$d" ] || continue
    name=${d%% *}
    if [ "$bundled_any" = 1 ] && printf '%s' "$name" | grep -Eiq "$STRIP_RE"; then
        continue
    fi
    if printf '%s' "$name" | grep -Eq "$FFMPEG_RE"; then
        d=$(relax_ffmpeg_floor "$d")
    fi
    kept="${kept:+$kept, }$d"
done
IFS=$OLDIFS
RUNTIME_DEPS="$kept"
echo "pack-deb: Depends: $RUNTIME_DEPS"

cat > "$PKGROOT/DEBIAN/control" <<EOF
Package: nordstjernen
Version: ${VERSION}
Architecture: ${DEBARCH}
Maintainer: Andreas Røsdal <andreas.rosdal@gmail.com>
Installed-Size: ${INSTALLED_KB}
Depends: ${RUNTIME_DEPS}
Section: web
Priority: optional
Homepage: https://nordstjernen.org
Description: Nordstjernen Sol Explorer — a small, hand-written web browser
 Nordstjernen is a small, free software web browser written in C with
 GTK 4 and libcurl. The HTML parser, CSS engine, layout, paint and
 JavaScript glue are written from scratch — no third-party browser engine
 is used. SVG images are rendered in-engine.
EOF

cat > "$PKGROOT/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -q -t /usr/share/icons/hicolor || true
fi
if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database -q /usr/share/applications || true
fi
EOF
cp "$PKGROOT/DEBIAN/postinst" "$PKGROOT/DEBIAN/postrm"
cat >> "$PKGROOT/DEBIAN/postinst" <<'EOF'
if [ "$1" = configure ]; then
    update-alternatives --remove internet-browser /usr/bin/nordstjernen \
        >/dev/null 2>&1 || true
    update-alternatives --install /usr/bin/open-browser-nordstjernen open-browser-nordstjernen \
        /usr/bin/nordstjernen 60
fi
EOF
cat > "$PKGROOT/DEBIAN/prerm" <<'EOF'
#!/bin/sh
set -e
if [ "$1" = remove ] || [ "$1" = deconfigure ]; then
    update-alternatives --remove open-browser-nordstjernen /usr/bin/nordstjernen
fi
EOF
chmod 755 "$PKGROOT/DEBIAN/postinst" "$PKGROOT/DEBIAN/postrm" \
    "$PKGROOT/DEBIAN/prerm"

DEB="$ROOT/dist/nordstjernen_${VERSION}_${DISTRO_TAG:+${DISTRO_TAG}_}${DEBARCH}.deb"
rm -f "$DEB"
dpkg-deb --root-owner-group --build "$PKGROOT" "$DEB" >/dev/null

echo
echo "Built: $DEB ($(du -h "$DEB" | cut -f1))"
echo
echo "Inspect: dpkg-deb -I $DEB"
echo "Contents: dpkg-deb -c $DEB"
echo "Install: sudo apt install $DEB"
