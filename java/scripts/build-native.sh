#!/usr/bin/env bash
#
# Build the engine (libnordstjernen) and the JNI bridge (libnordstjernenjni),
# staging both into java/src/main/resources/native/<os>-<arch>/ so the Gradle
# jar bundles them and NativeLoader can extract them at runtime.
#
# Requires: JAVA_HOME (JDK 21), meson + ninja, and the engine's build deps
# (gtk4/cairo/pango/curl/...). See the repo README.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILDDIR="${BUILDDIR:-$REPO_ROOT/builddir-java}"
: "${JAVA_HOME:?set JAVA_HOME to a JDK 21}"

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

if [ ! -e "$BUILDDIR/build.ninja" ]; then
    meson setup "$BUILDDIR" "$REPO_ROOT" -Ddefault_library=shared
fi
meson compile -C "$BUILDDIR"
release_build_or_die "$BUILDDIR"

OS="$(uname -s)"
ARCH="$(uname -m)"
PIC=-fPIC
case "$OS" in
    Linux)  P_OS=linux   ; SOEXT=so    ; JNI_MD=linux  ; RPATH='-Wl,-rpath,$ORIGIN'      ;;
    Darwin) P_OS=macos   ; SOEXT=dylib ; JNI_MD=darwin ; RPATH='-Wl,-rpath,@loader_path' ;;
    MINGW*|MSYS*)
            P_OS=windows ; SOEXT=dll   ; JNI_MD=win32  ; RPATH='' ; PIC=''               ;;
    *)      P_OS=linux   ; SOEXT=so    ; JNI_MD=linux  ; RPATH='-Wl,-rpath,$ORIGIN'      ;;
esac
case "$ARCH" in
    x86_64|amd64)  P_ARCH=x86_64  ;;
    aarch64|arm64) P_ARCH=aarch64 ;;
    *)             P_ARCH="$ARCH" ;;
esac
PLAT="$P_OS-$P_ARCH"

ENGINE="$(ls "$BUILDDIR"/src/libnordstjernen.$SOEXT 2>/dev/null | head -1 || true)"
if [ -z "$ENGINE" ]; then
    echo "engine library not found in $BUILDDIR/src" >&2
    exit 1
fi

JNI_MD_DIR="$JAVA_HOME/include/$JNI_MD"
[ -d "$JNI_MD_DIR" ] || JNI_MD_DIR="$JAVA_HOME/include/linux"

JNI_LIB="libnordstjernenjni.$SOEXT"
[ "$P_OS" = windows ] && JNI_LIB="nordstjernenjni.dll"

OUTDIR="$REPO_ROOT/java/src/main/resources/native/$PLAT"
mkdir -p "$OUTDIR"

CC="${CC:-cc}"
$CC -shared $PIC -O2 -Wall -Wextra \
    -I"$JAVA_HOME/include" -I"$JNI_MD_DIR" -I"$REPO_ROOT/src" \
    "$REPO_ROOT/java/src/main/native/ns_java_jni.c" \
    -L"$(dirname "$ENGINE")" -lnordstjernen \
    $RPATH \
    -o "$OUTDIR/$JNI_LIB"

cp -f "$ENGINE" "$OUTDIR/libnordstjernen.$SOEXT"

echo "Staged libnordstjernen.$SOEXT + $JNI_LIB into $OUTDIR"
