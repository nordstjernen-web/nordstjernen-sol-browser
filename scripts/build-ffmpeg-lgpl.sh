#!/usr/bin/env bash
# Build a minimal, LGPL-licensed FFmpeg (libav*) carrying exactly the inline
# media Nordstjernen decodes — VP9/VP8/Theora video and Opus/Vorbis audio in Matroska
# /WebM and Ogg containers — so it can be bundled into the redistributable
# macOS and Windows packages without the GPL obligations of a stock FFmpeg.
# It also demuxes MP4 and parses H.264/AAC but carries no H.264 or AAC
# decoder: those streams are decoded by the operating system (Media
# Foundation, VideoToolbox; see src/media_os.h).
#
# `--disable-gpl --disable-nonfree --disable-version3` keeps the result LGPL
# v2.1+, and `--disable-autodetect` guarantees no external codec library
# (libx264, libx265, …) is linked — the enabled VP8/VP9/Theora/Opus/Vorbis decoders
# are FFmpeg's own native, LGPL implementations.
#
# Usage: scripts/build-ffmpeg-lgpl.sh <install-prefix>
# Prints the prefix on success; point pkg-config at "<prefix>/lib/pkgconfig".
#
# Env:
#   FFMPEG_VERSION   release tag without the leading n (default 8.1.3)
#   NS_FFMPEG_NOASM  set to 1 to configure --disable-x86asm (no nasm needed)
#   NS_FFMPEG_HOST   cross prefix passed as --cross-prefix (e.g. for mingw)
#   FFMPEG_EXTRA_CONFIGURE  extra configure flags (target-os/arch on cross)
set -euo pipefail

PREFIX=${1:?usage: build-ffmpeg-lgpl.sh <install-prefix>}
VER=${FFMPEG_VERSION:-8.1.3}
JOBS=${NS_BUILD_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

log() { printf '[ffmpeg-lgpl] %s\n' "$*" >&2; }

case "$VER" in
    8.1.3) SHA256=bd458826a039b48a9606e794554c75eb4c4984b84173f7afa3128eae89336f2b ;;
    *)     SHA256=${FFMPEG_SHA256:-} ;;
esac
[ -n "$SHA256" ] || { log "ERROR: no pinned checksum for FFmpeg ${VER}; set FFMPEG_SHA256"; exit 1; }

sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | cut -d' ' -f1
    else
        shasum -a 256 "$1" | cut -d' ' -f1
    fi
}

TARBALL="$WORK/ffmpeg-${VER}.tar.gz"
url="https://ffmpeg.org/releases/ffmpeg-${VER}.tar.gz"
log "fetching $url"
curl -fsSL --retry 3 --max-time 300 -o "$TARBALL" "$url" ||
    { log "ERROR: could not download FFmpeg ${VER}"; exit 1; }
got=$(sha256_of "$TARBALL")
[ "$got" = "$SHA256" ] ||
    { log "ERROR: checksum mismatch for $url (got $got)"; exit 1; }

SRC="$WORK/ffmpeg-src"
mkdir -p "$SRC"
tar -xzf "$TARBALL" -C "$SRC" --strip-components=1

ASM_FLAG=--enable-x86asm
[ "${NS_FFMPEG_NOASM:-0}" = 1 ] && ASM_FLAG=--disable-x86asm

CROSS=()
[ -n "${NS_FFMPEG_HOST:-}" ] && CROSS+=(--cross-prefix="${NS_FFMPEG_HOST}-" --enable-cross-compile)

cd "$SRC"
# shellcheck disable=SC2086
./configure \
    --prefix="$PREFIX" \
    --disable-gpl --disable-nonfree --disable-version3 \
    --disable-autodetect \
    --disable-static --enable-shared \
    --disable-programs --disable-doc --disable-htmlpages --disable-manpages \
    --disable-debug --enable-pic \
    --disable-network --disable-avdevice --disable-avfilter \
    --disable-everything \
    --enable-avformat --enable-avcodec --enable-avutil \
    --enable-swscale --enable-swresample \
    --enable-demuxer=matroska,ogg,mov,flac,wav \
    --enable-decoder=vp8,vp9,theora,opus,vorbis,flac,pcm_s16le,pcm_s24le,pcm_u8,pcm_f32le \
    --enable-parser=vp8,vp9,vp3,opus,vorbis,flac,h264,aac \
    --enable-protocol=file \
    $ASM_FLAG \
    "${CROSS[@]}" \
    ${FFMPEG_EXTRA_CONFIGURE:-}

make -j"$JOBS"
make install

# Reject any GPL/nonfree slip-up: config.h records the license decisions.
if grep -Eq '^#define CONFIG_(GPL|NONFREE) 1' config.h; then
    log "ERROR: built FFmpeg is not LGPL (GPL/nonfree enabled)"; exit 1
fi

log "installed LGPL FFmpeg ${VER} to $PREFIX"
printf '%s\n' "$PREFIX"
