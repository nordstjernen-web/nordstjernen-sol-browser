#!/usr/bin/env bash
# Build and package Nordstjernen inside a distro container. Invoked by
# nightly.sh via `docker run` with the source tree at the current
# directory; installs that distro's deps, builds a release binary, and
# emits a portable tarball plus a native package (.deb or .rpm) under
# dist/. Argument 1 selects the distro: debian | ubuntu | fedora | opensuse |
# alpine.
set -euo pipefail

DISTRO=${1:?usage: nightly-distro-build.sh <debian|ubuntu|fedora|opensuse|alpine>}
export VERSION=${VERSION:-}
export DEBIAN_FRONTEND=noninteractive
export CC=${CC:-cc}

trap 'rc=$?; echo "nightly-distro-build($DISTRO): FAILED (exit $rc) at line $LINENO: $BASH_COMMAND" >&2; exit $rc' ERR

install_apt() {
    apt-get update -qq
    apt-get install -y --no-install-recommends \
        build-essential clang pkg-config ninja-build cmake git zip unzip curl \
        python3-pip dpkg-dev patchelf ca-certificates \
        libgtk-4-dev libepoxy-dev libcurl4-openssl-dev libssl-dev libuchardet-dev libpsl-dev \
        libsqlite3-dev libseccomp-dev libwebp-dev libavif-dev libsdl2-dev
    apt-get install -y --no-install-recommends \
        libpoppler-glib-dev \
        libfontconfig-dev libpango1.0-dev libavif-dev || true
    # FFmpeg libav* backs the inline WebM path (VP9/VP8 video + Opus/Vorbis
    # audio). meson requires it on Linux, so this is not optional — its own
    # line to keep the failure legible when a mirror is missing the packages.
    apt-get install -y --no-install-recommends \
        libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libswresample-dev \
        libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev
    pip3 install --break-system-packages --upgrade 'meson>=1.4' \
        || pip3 install --upgrade 'meson>=1.4'
}

install_zypper() {
    # Tumbleweed is a rolling release and its mirrors are frequently caught
    # mid-sync, so a repo's repomd.xml can transiently reference metadata
    # files (e.g. appdata.xml.gz) not yet present on the mirror, making
    # `zypper refresh` fail. Retry a few times — the sync window resolves in
    # a minute or two and download.opensuse.org rotates mirrors on retry —
    # then fall through: `zypper install` auto-refreshes, so a still-stale
    # repo fails there with a real error instead of a single flake aborting
    # the whole nightly stage.
    local i
    for i in 1 2 3 4 5; do
        zypper --non-interactive --gpg-auto-import-keys refresh && break
        echo "zypper refresh attempt $i failed (mirror likely mid-sync); retrying in $((i * 15))s..." >&2
        sleep $((i * 15))
    done
    zypper --non-interactive --gpg-auto-import-keys install --no-recommends \
        gcc gcc-c++ clang pkgconf-pkg-config meson ninja cmake git zip unzip curl \
        rpm-build patchelf ca-certificates \
        gtk4-devel libepoxy-devel libcurl-devel libopenssl-devel libuchardet-devel libpsl-devel \
        sqlite3-devel libseccomp-devel libwebp-devel libavif-devel
    # SDL2 backs the auto-detected audio helper; keep it out of the required
    # set so an unavailable/mid-sync package degrades to no audio, not a failed
    # nightly. Its own line (not the optional group) so it is independent of
    # poppler availability.
    zypper --non-interactive --gpg-auto-import-keys install --no-recommends \
        'pkgconfig(sdl2)' \
        || echo "nightly-distro-build(opensuse): SDL2 unavailable; audio helper skipped" >&2
    zypper --non-interactive --gpg-auto-import-keys install --no-recommends \
        libpoppler-glib-devel \
        fontconfig-devel pango-devel libavif-devel || true
    # FFmpeg libav* (inline WebM: VP9/VP8 + Opus/Vorbis). meson requires it on
    # Linux, so a failure here is fatal; if the default repos come up short,
    # the container needs Packman enabled.
    zypper --non-interactive --gpg-auto-import-keys install --no-recommends \
        libavformat-devel libavcodec-devel libavutil-devel libswscale-devel libswresample-devel \
        'pkgconfig(gstreamer-1.0)' 'pkgconfig(gstreamer-app-1.0)' 'pkgconfig(gstreamer-video-1.0)'
}

install_dnf() {
    # curl replaces the image's curl-minimal, hence --allowerasing.
    dnf install -y --setopt=install_weak_deps=False --allowerasing \
        gcc gcc-c++ clang pkgconf-pkg-config meson ninja-build cmake git zip unzip curl \
        gawk findutils diffutils which rpm-build patchelf ca-certificates \
        gtk4-devel libepoxy-devel libcurl-devel openssl-devel uchardet-devel libpsl-devel \
        sqlite-devel libseccomp-devel libwebp-devel libavif-devel SDL2-devel
    # Fedora's FFmpeg is ffmpeg-free, which carries no H.264/AAC decoder, so
    # GStreamer is what plays MP4 there.
    dnf install -y --setopt=install_weak_deps=False \
        poppler-glib-devel gstreamer1-devel gstreamer1-plugins-base-devel \
        fontconfig-devel pango-devel libjxl-devel enchant2-devel || true
    # FFmpeg libav* (inline WebM: VP9/VP8 + Opus/Vorbis) from Fedora's own
    # ffmpeg-free packages. The SONAMEs the RPM then requires are the ones this
    # Fedora release ships (FFmpeg 8 on 44, FFmpeg 9 on 45), which is why each
    # release gets its own RPM. meson requires them on Linux, so this is fatal.
    dnf install -y --setopt=install_weak_deps=False \
        'pkgconfig(libavformat)' 'pkgconfig(libavcodec)' 'pkgconfig(libavutil)' \
        'pkgconfig(libswscale)' 'pkgconfig(libswresample)'
}

install_apk() {
    apk update -q
    apk add --no-cache \
        build-base clang pkgconf meson ninja cmake git zip alpine-sdk \
        linux-headers gtk4.0-dev libepoxy-dev curl-dev openssl-dev uchardet-dev libpsl-dev sqlite-dev \
        libseccomp-dev libwebp-dev sdl2-dev
    apk add --no-cache \
        poppler-dev \
        fontconfig-dev freetype-dev pango-dev brotli-dev libavif-dev libjxl-dev \
        gstreamer-dev gst-plugins-base-dev enchant2-dev libvorbis-dev opusfile-dev || true
    # FFmpeg libav* (inline WebM: VP9/VP8 + Opus/Vorbis). ffmpeg-dev provides
    # all of libavformat/libavcodec/libavutil/libswscale/libswresample, and
    # meson requires them on Linux.
    apk add --no-cache ffmpeg-dev
}

case "$DISTRO" in
    debian|ubuntu) install_apt ;;
    fedora)        install_dnf ;;
    opensuse)      install_zypper ;;
    alpine)        install_apk ;;
    *) echo "unknown distro: $DISTRO" >&2; exit 2 ;;
esac

git config --global --add safe.directory "$(pwd)" || true

if [ -z "${NS_BUILD_JOBS:-}" ]; then
    mem_gb=$(awk '/MemTotal/{print int($2/1024/1024)}' /proc/meminfo)
    cores=$(nproc)
    jobs=$(( mem_gb / 2 ))
    [ "$jobs" -lt 2 ] && jobs=2
    [ "$jobs" -gt "$cores" ] && jobs=$cores
    export NS_BUILD_JOBS=$jobs
fi
export NS_BUILD_LTO=${NS_BUILD_LTO:-false}
echo "nightly-distro-build($DISTRO): building with -j${NS_BUILD_JOBS} lto=${NS_BUILD_LTO} (mem-bounded)"

./scripts/pack-linux.sh

case "$DISTRO" in
    debian|ubuntu)   ./scripts/pack-deb.sh ;;
    fedora|opensuse) ./scripts/pack-rpm.sh ;;
    alpine)          ./scripts/pack-apk.sh ;;
esac

# Install the native package into this same container and run the installed
# browser headlessly. That proves the control metadata parses, the maintainer
# scripts run, every declared dependency resolves against a real package
# database, and the installed binaries start from their packaged paths — so a
# package that would fail at `dpkg -i` on a user's machine fails here, where
# nightly.sh then publishes only the portable zip. (Dependency floors are
# checked against this container's own packages, so a floor that is merely too
# strict for older releases is pack-deb.sh's job to relax, not this check's.)
verify_package() {
    local pkg="$1"
    echo "nightly-distro-build($DISTRO): installing $(basename "$pkg") to verify it"
    case "$DISTRO" in
        debian|ubuntu)
            dpkg -i "$pkg"
            dpkg-deb -f "$pkg" Depends | sed 's/^/  Depends: /'
            ;;
        fedora)
            # dnf resolves the Requires against the real Fedora repositories,
            # so an RPM that would fail to install on a user's machine of this
            # release fails here.
            dnf install -y --setopt=install_weak_deps=False "$pkg"
            rpm -qp --requires "$pkg" | sed 's/^/  Requires: /'
            ;;
        opensuse)
            rpm -Uvh --replacepkgs "$pkg"
            rpm -qp --requires "$pkg" | sed 's/^/  Requires: /'
            ;;
        alpine)
            apk add --allow-untrusted "$pkg"
            ;;
    esac
    # The container runs as root, which the browser refuses without opt-in.
    NS_ALLOW_ROOT=1 /usr/bin/nordstjernen --headless --dump=text about:start \
        > /tmp/ns-smoke.txt
    [ -s /tmp/ns-smoke.txt ] || {
        echo "nightly-distro-build($DISTRO): installed browser produced no output for about:start" >&2
        return 1
    }
    echo "nightly-distro-build($DISTRO): installed package smoke test passed:"
    head -n 5 /tmp/ns-smoke.txt | sed 's/^/  | /'
}
shopt -s nullglob
pkgs=(dist/*.deb dist/*.rpm dist/*.apk)
shopt -u nullglob
if [ ${#pkgs[@]} -gt 0 ]; then
    verify_package "${pkgs[0]}"
fi

echo
echo "nightly-distro-build($DISTRO): artifacts in dist/:"
ls -1 dist/*.zip dist/*.deb dist/*.rpm dist/*.apk 2>/dev/null || true
