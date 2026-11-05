"""What this package installs: the Nordstjernen release, its build tools and the libraries its build requires."""

import sys

from . import __version__

VERSION = __version__
NAME = "Nordstjernen"
PACKAGE_NAME = "nordstjernen-browser"
ENV_PREFIX = "NORDSTJERNEN"
REPO = "nordstjernen-web/nordstjernen-sol-browser"
SOURCE_DIRECTORY = f"nordstjernen-sol-browser-{VERSION}"
EXECUTABLE = "bin/nordstjernen.exe" if sys.platform == "win32" else "bin/nordstjernen"
MESON_OPTIONS = ["--buildtype=release"]
WINDOWS_ENV = {"CC": "clang"}
SUPPORTED_DISTRIBUTIONS = ("Ubuntu 24.04, Debian 13, Fedora 40, openSUSE Tumbleweed, "
                           "Arch Linux, Alpine 3.21 or newer")
MAC_RELEASE = {
    "asset": f"nordstjernen-{VERSION}-macos-arm64.dmg",
    "app": "Nordstjernen.app",
    "executable": "Nordstjernen.app/Contents/MacOS/Nordstjernen",
}

TOOLS = [
    {"label": "meson", "probe": ["meson"], "minimum": "1.0",
     "upgrade": ["A newer meson can be installed for your user with:", "",
                 "  python3 -m pip install --user meson"],
     "apt": "meson", "dnf": "meson", "zypper": "meson", "pacman": "meson", "apk": "meson",
     "brew": "meson", "msys": "meson"},
    {"label": "ninja", "probe": ["ninja", "samu"],
     "apt": "ninja-build", "dnf": "ninja-build", "zypper": "ninja", "pacman": "ninja",
     "apk": "samurai", "brew": "ninja", "msys": "ninja"},
    {"label": "pkg-config", "probe": ["pkg-config", "pkgconf"],
     "apt": "pkgconf", "dnf": "pkgconf", "zypper": "pkgconf", "pacman": "pkgconf",
     "apk": "pkgconf", "brew": "pkgconf", "msys": "pkgconf"},
    {"label": "tar", "probe": ["tar"],
     "apt": "tar", "dnf": "tar", "zypper": "tar", "pacman": "tar", "apk": "tar", "msysBase": "tar"},
    {"label": "a C compiler (gcc or clang)", "probe": ["cc", "gcc", "clang"], "compiler": True,
     "apt": "build-essential", "dnf": "gcc", "zypper": "gcc", "pacman": "base-devel",
     "apk": "build-base", "msys": "clang"},
]


def _lib(module, apt, pacman, brew=None, msys=None, **extra):
    entry = {"module": module, "apt": apt, "pacman": pacman}
    if brew:
        entry["brew"] = brew
    if msys:
        entry["msys"] = msys
    entry.update(extra)
    return entry


REQUIRED = [
    _lib("glib-2.0", "libglib2.0-dev", "glib2", "glib", "glib2", minimum="2.80"),
    _lib("gtk4", "libgtk-4-dev", "gtk4", "gtk4", "gtk4",
         minimum="4.22.1" if sys.platform == "win32" else "4.14"),
    _lib("epoxy", "libepoxy-dev", "libepoxy", "libepoxy", "libepoxy"),
    _lib("libcurl", "libcurl4-openssl-dev", "curl", "curl", "curl", minimum="8.5"),
    _lib("libcrypto", "libssl-dev", "openssl", "openssl@3", "openssl"),
    _lib("libssl", "libssl-dev", "openssl", "openssl@3", "openssl"),
    _lib("uchardet", "libuchardet-dev", "uchardet", "uchardet", "uchardet"),
    _lib("libpsl", "libpsl-dev", "libpsl", "libpsl", "libpsl"),
    _lib("sqlite3", "libsqlite3-dev", "sqlite", "sqlite", "sqlite3"),
    _lib("libwebp", "libwebp-dev", "libwebp", "webp", "libwebp"),
    _lib("libwebpdemux", "libwebp-dev", "libwebp", "webp", "libwebp"),
    _lib("zlib", "zlib1g-dev", "zlib", "zlib", "zlib"),
    _lib("fribidi", "libfribidi-dev", "fribidi", "fribidi", "fribidi", minimum="1.0.6"),
    _lib("harfbuzz", "libharfbuzz-dev", "harfbuzz", "harfbuzz", "harfbuzz", minimum="8.3.0"),
    _lib("fontconfig", "libfontconfig-dev", "fontconfig", "fontconfig", "fontconfig",
         minimum="2.15.0"),
    _lib("freetype2", "libfreetype-dev", "freetype2", "freetype", "freetype"),
    _lib("cairo", "libcairo2-dev", "cairo", "cairo", "cairo", minimum="1.18.0"),
    _lib("cairo-ft", "libcairo2-dev", "cairo", "cairo", "cairo", minimum="1.18.0"),
    _lib("libseccomp", "libseccomp-dev", "libseccomp", linux=True),
] + [
    _lib(module, f"{module}-dev", "ffmpeg", msys="ffmpeg", minimum=minimum,
         platforms=["linux", "win32"],
         note="FFmpeg 6.0 or newer; on Fedora/RHEL from RPM Fusion, on openSUSE from Packman")
    for module, minimum in [("libavformat", "60"), ("libavcodec", "60"), ("libavutil", "58"),
                            ("libswscale", "7"), ("libswresample", "4")]
]

OPTIONAL = [
    _lib("glslang", "glslang-dev", "glslang", "glslang", "glslang",
         feature="WebGL", meson_option="glslang"),
    _lib("sdl2", "libsdl2-dev", "sdl2", "sdl2", "SDL2", feature="audio playback"),
    _lib("gstreamer-1.0", "libgstreamer1.0-dev", "gstreamer", minimum="1.20", unix=True,
         feature="H.264 video and AAC audio", meson_option="gstreamer"),
    _lib("gstreamer-app-1.0", "libgstreamer-plugins-base1.0-dev", "gst-plugins-base-libs",
         minimum="1.20", unix=True, feature="H.264 video and AAC audio", meson_option="gstreamer"),
    _lib("gstreamer-video-1.0", "libgstreamer-plugins-base1.0-dev", "gst-plugins-base-libs",
         minimum="1.20", unix=True, feature="H.264 video and AAC audio", meson_option="gstreamer"),
    _lib("poppler-glib", "libpoppler-glib-dev", "poppler-glib", "poppler", "poppler",
         feature="viewing PDF files"),
    _lib("libbrotlidec", "libbrotli-dev", "brotli", "brotli", "brotli", feature="WOFF2 web fonts"),
    _lib("enchant-2", "libenchant-2-dev", "enchant", "enchant", "enchant", feature="spell-checking"),
    _lib("libavif", "libavif-dev", "libavif", "libavif", "libavif", feature="AVIF images"),
    _lib("libjxl", "libjxl-dev", "libjxl", "jpeg-xl", "libjxl", feature="JPEG XL images"),
    {"module": "libavcodec", "minimum": "60", "mac": True, "feature": "WebM video", "brew": "ffmpeg"},
]
