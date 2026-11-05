#!/usr/bin/env bash
# Build a redistributable Nordstjernen Windows bundle: a root launcher plus the
# MSYS2 DLLs and GTK runtime data under app/ so it runs outside MSYS2.
#
# Builds (or reuses) a separate --buildtype=release tree in $BUILDDIR so the
# shipped binary has NDEBUG defined — third-party assertions in vendored deps
# like quickjs-ng are compiled out, and the optimiser runs at -O3.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILDDIR=${BUILDDIR:-$ROOT/builddir-release}
OUT=${OUT:-$ROOT/dist/nordstjernen-win64}
APP=$OUT/app
BIN_SRC=$BUILDDIR/src/gtk/nordstjernen.exe
RENDERER_SRC=$BUILDDIR/src/nordstjernen-renderer.exe
LAUNCHER_SRC=$BUILDDIR/src/nordstjernen-launcher.exe
AUDIO_SRC=$BUILDDIR/src/nordstjernen-audio.exe
BROWSER_EXE=nordstjernen-ui.exe
EXTRA_MESON_SETUP_ARGS=()
if [ -n "${NS_MESON_SETUP_ARGS:-}" ]; then
    EXTRA_MESON_SETUP_ARGS=($NS_MESON_SETUP_ARGS)
fi

resolve_mingw_prefix() {
    for cand in "${MINGW_PREFIX:-}" /c/msys64/ucrt64 "C:/msys64/ucrt64" \
                /ucrt64 /c/msys64/mingw64 "C:/msys64/mingw64" /mingw64; do
        [ -n "$cand" ] || continue
        [ -d "$cand/bin" ] || continue
        [ -f "$cand/bin/libgtk-4-1.dll" ] || continue
        echo "$cand"; return
    done
    return 1
}

MINGW_PREFIX=$(resolve_mingw_prefix) || {
    echo "pack-windows: could not find the MSYS2 bin directory; set MINGW_PREFIX." >&2
    exit 1
}

release_build_or_die() {
    local cfg
    cfg=$(meson configure "$1" 2>/dev/null |
          awk '$1=="buildtype"||$1=="debug"||$1=="b_sanitize"{v=$2; if (v=="[]" || v=="[none]") v="none"; printf " %s=%s", $1, v}')
    case "$cfg " in
        *" buildtype=release "*|*" buildtype=minsize "*) ;;
        *) echo "$(basename "$0"): $1 is not a release build ($cfg ); refusing to package it" >&2
           exit 1 ;;
    esac
    case "$cfg " in
        *" debug=true "*|*" b_sanitize="[!n]*)
           echo "$(basename "$0"): $1 carries debug info or sanitizers ($cfg ); refusing to package it" >&2
           exit 1 ;;
    esac
}

if [ ! -d "$BUILDDIR" ]; then
    meson setup "$BUILDDIR" --buildtype=release "${EXTRA_MESON_SETUP_ARGS[@]}"
elif [ ${#EXTRA_MESON_SETUP_ARGS[@]} -gt 0 ]; then
    meson configure "$BUILDDIR" "${EXTRA_MESON_SETUP_ARGS[@]}"
fi
meson compile -C "$BUILDDIR"
release_build_or_die "$BUILDDIR"

if [ ! -x "$BIN_SRC" ]; then
    echo "pack-windows: build did not produce $BIN_SRC" >&2
    exit 1
fi
if [ ! -x "$LAUNCHER_SRC" ]; then
    echo "pack-windows: build did not produce $LAUNCHER_SRC" >&2
    exit 1
fi
if [ ! -x "$RENDERER_SRC" ]; then
    echo "pack-windows: build did not produce $RENDERER_SRC" >&2
    exit 1
fi

rm -rf "$OUT"
mkdir -p "$APP"
cp "$LAUNCHER_SRC" "$OUT/nordstjernen.exe"
cp "$BIN_SRC" "$APP/$BROWSER_EXE"
# The browser spawns one sandboxed renderer process per tab; ship it next to
# the browser exe so it is discovered without NS_RENDERER.
cp "$RENDERER_SRC" "$APP/nordstjernen-renderer.exe"
# Audio playback helper (MP2/MP3 decode + SDL2 output). Built whenever SDL2
# was present at configure time; ship it beside the browser exe so the shell
# finds it (ns_proc_audio_helper_path) and seed the DLL chase below with it so
# SDL2.dll and its transitive deps are bundled (nothing else links SDL2).
if [ -x "$AUDIO_SRC" ]; then
    cp "$AUDIO_SRC" "$APP/nordstjernen-audio.exe"
else
    echo "pack-windows: warning: $AUDIO_SRC missing; <video>/<audio> sound will not play" >&2
fi

validate_launcher_imports() {
    local dep src alt
    while IFS= read -r dep; do
        [ -n "$dep" ] || continue
        src=$MINGW_PREFIX/bin/$dep
        if [ ! -f "$src" ]; then
            alt=$(find "$MINGW_PREFIX/bin" -maxdepth 1 -iname "$dep" -print -quit 2>/dev/null || true)
            [ -n "$alt" ] && src=$alt
        fi
        if [ -f "$src" ]; then
            printf 'pack-windows: root launcher imports %s; keep it system-only before using the app/ layout\n' \
                "$dep" >&2
            exit 1
        fi
    done < <(objdump -p "$OUT/nordstjernen.exe" 2>/dev/null | awk '/DLL Name:/ {print $3}')
}

validate_launcher_imports

# IANA time zone database for Temporal's named zones (the posix/ and right/
# duplicates are left out).
mkdir -p "$APP/share/zoneinfo"
if [ -f "$MINGW_PREFIX/share/zoneinfo/tzdata.zi" ]; then
    (cd "$MINGW_PREFIX/share/zoneinfo" && tar cf - --exclude=./posix --exclude=./right .) |
        (cd "$APP/share/zoneinfo" && tar xf -)
else
    echo "pack-windows: warning: no zoneinfo in $MINGW_PREFIX/share; Temporal will know only UTC" >&2
fi

# GLib settings schemas (compiled). Apps that GSettings-look-up a key crash without these.
mkdir -p "$APP/share/glib-2.0/schemas"
if [ -f "$MINGW_PREFIX/share/glib-2.0/schemas/gschemas.compiled" ]; then
    cp "$MINGW_PREFIX/share/glib-2.0/schemas/gschemas.compiled" "$APP/share/glib-2.0/schemas/"
fi

if [ -d "$MINGW_PREFIX/etc/fonts" ]; then
    mkdir -p "$APP/etc"
    cp -r "$MINGW_PREFIX/etc/fonts" "$APP/etc/"
fi

# GDK-PixBuf loader cache + loader DLLs (image decode for <img>). Copied
# *before* the DLL chase so the loaders' transitive deps (notably
# librsvg-2-2.dll, pulled in only by pixbufloader_svg.dll) get bundled too.
# GdkPixbuf loads these dynamically via loaders.cache, so their import edges
# don't appear in nordstjernen.exe's static-import graph.
if [ -d "$MINGW_PREFIX/lib/gdk-pixbuf-2.0" ]; then
    mkdir -p "$APP/lib"
    cp -r "$MINGW_PREFIX/lib/gdk-pixbuf-2.0" "$APP/lib/"
fi

# Transitively resolve DLL dependencies starting from the exe and every
# pixbuf loader DLL. objdump reports import names; we look them up in the
# mingw bin dir and skip anything that resolves to a Windows system DLL.
declare -A seen
queue=("$APP/$BROWSER_EXE" "$APP/nordstjernen-renderer.exe")
[ -f "$APP/nordstjernen-audio.exe" ] && queue+=("$APP/nordstjernen-audio.exe")
for loader in "$APP"/lib/gdk-pixbuf-2.0/*/loaders/*.dll; do
    [ -f "$loader" ] && queue+=("$loader")
done
while [ ${#queue[@]} -gt 0 ]; do
    cur=${queue[0]}
    queue=("${queue[@]:1}")
    deps=$(objdump -p "$cur" 2>/dev/null | awk '/DLL Name:/ {print $3}') || true
    for dep in $deps; do
        key=$(printf '%s' "$dep" | tr '[:upper:]' '[:lower:]')
        if [ -n "${seen[$key]:-}" ]; then continue; fi
        seen[$key]=1
        src=$MINGW_PREFIX/bin/$dep
        if [ ! -f "$src" ]; then
            # case-insensitive fallback for DLLs whose import name capitalisation
            # differs from the on-disk file name
            alt=$(find "$MINGW_PREFIX/bin" -maxdepth 1 -iname "$dep" -print -quit 2>/dev/null || true)
            [ -n "$alt" ] && src=$alt
        fi
        if [ -f "$src" ]; then
            cp "$src" "$APP/"
            queue+=("$APP/$(basename "$src")")
        fi
    done
done

dll_exports_symbol() {
    objdump -p "$1" 2>/dev/null | awk -v want="$2" '
        /^[[:space:]]*\[[[:space:]]*[0-9]+\][[:space:]]/ && $NF == want {
            found = 1
        }
        END { exit found ? 0 : 1 }
    '
}

validate_ngtcp2_ossl() {
    local ngtcp2 ssl_dep ssl
    ngtcp2=$(find "$APP" -maxdepth 1 -iname 'libngtcp2_crypto_ossl-0.dll' -print -quit 2>/dev/null || true)
    [ -n "$ngtcp2" ] || return 0
    ssl_dep=$(objdump -p "$ngtcp2" 2>/dev/null | awk '
        /DLL Name:/ { dep = $3; next }
        dep != "" && $NF == "SSL_set_quic_tls_cbs" { print dep; exit }
    ')
    [ -n "$ssl_dep" ] || return 0
    ssl=$(find "$APP" -maxdepth 1 -iname "$ssl_dep" -print -quit 2>/dev/null || true)
    if [ -z "$ssl" ]; then
        printf 'pack-windows: %s imports SSL_set_quic_tls_cbs from missing %s\n' \
            "$(basename "$ngtcp2")" "$ssl_dep" >&2
        exit 1
    fi
    if ! dll_exports_symbol "$ssl" SSL_set_quic_tls_cbs; then
        printf 'pack-windows: %s imports SSL_set_quic_tls_cbs, but bundled %s does not export it\n' \
            "$(basename "$ngtcp2")" "$(basename "$ssl")" >&2
        exit 1
    fi
}

validate_ngtcp2_ossl

# Adwaita + hicolor icons for default GTK widget glyphs (back/forward arrows, etc.).
mkdir -p "$APP/share/icons"
for theme in Adwaita hicolor; do
    if [ -d "$MINGW_PREFIX/share/icons/$theme" ]; then
        cp -r "$MINGW_PREFIX/share/icons/$theme" "$APP/share/icons/"
    fi
done

# Nordstjernen's own application + toolbar icons (drop into the hicolor theme
# so gtk_image_new_from_icon_name("nordstjernen-back") and friends resolve at
# runtime, and about: pages can read the svg/gif as a data URI). The dev tree
# finds these via the ../../data/icons search path; the bundle only has
# share/icons, so every nordstjernen-*.svg the toolbar references must be
# copied there or the header-bar buttons render blank. Refresh the hicolor
# cache so the bundled icons show up without a filesystem scan.
mkdir -p "$APP/share/icons/hicolor/scalable/apps"
cp "$ROOT"/data/icons/hicolor/scalable/apps/nordstjernen*.svg \
   "$ROOT/data/icons/hicolor/scalable/apps/nordstjernen.gif" \
   "$APP/share/icons/hicolor/scalable/apps/"
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache --force --ignore-theme-index \
        "$APP/share/icons/hicolor" >/dev/null 2>&1 || true
fi

# Per-application data: license texts. The browser reads them relative to
# the exe at runtime (see src/net.c::about_read_first).
mkdir -p "$APP/share/nordstjernen"
cp "$ROOT/License.md" "$APP/share/nordstjernen/"
cp "$ROOT/COPYING" "$APP/share/nordstjernen/"

# Third-party copyright + license notices required by the libraries we ship.
cp "$ROOT/THIRD-PARTY-LICENSES.md" "$OUT/"

# CA certificate bundle for libcurl HTTPS verification.
mkdir -p "$APP/etc/ssl/certs"
for ca in \
    "$MINGW_PREFIX/etc/ssl/certs/ca-bundle.crt" \
    "$MINGW_PREFIX/etc/ssl/cert.pem" \
    "$MINGW_PREFIX/ssl/certs/ca-bundle.crt"; do
    if [ -f "$ca" ]; then
        cp "$ca" "$APP/etc/ssl/certs/ca-bundle.crt"
        break
    fi
done

"$ROOT/scripts/check-no-profile-data.sh" "$OUT"
bundled=$(find "$APP" -maxdepth 1 -name '*.dll' | wc -l)
size=$(du -sh "$OUT" | awk '{print $1}')
printf 'pack-windows: bundled %s DLLs, total size %s, output: %s\n' "$bundled" "$size" "$OUT"
