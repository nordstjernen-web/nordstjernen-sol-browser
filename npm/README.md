# nordstjernen-browser

[Nordstjernen Web Navigator](https://nordstjernen.org) is a minimal, secure
web browser written from scratch in C with GTK 4.

![Nordstjernen Web Navigator](https://raw.githubusercontent.com/nordstjernen-web/nordstjernen-sol-browser/main/docs/screenshot.png)

```sh
npm install -g nordstjernen-browser
nordstjernen https://nordstjernen.org
```

## What installing does

| System | What happens |
| --- | --- |
| Linux, BSD, Intel macOS | Nordstjernen is built from the release source on your machine with meson and ninja, against your system's GTK 4, FFmpeg and other libraries. The source tarball is checked against the SHA-256 recorded in this package. The build takes several minutes and shows its progress in the terminal. |
| macOS on Apple silicon | The Developer ID signed and notarised release build is downloaded from GitHub and checked against the release's `SHA256SUMS`. |
| Windows | Nordstjernen is built from the release source with [MSYS2](https://www.msys2.org)'s UCRT64 clang, meson and ninja, against MSYS2's GTK 4 and FFmpeg. The signed build is in the [Microsoft Store](https://apps.microsoft.com/detail/9NW8T7W5Z4PL). |

The browser is installed inside the npm package, so `npm uninstall -g
nordstjernen-browser` removes it. If that directory is not writable, the
browser goes into `~/.cache/nordstjernen-browser` (`~/Library/Caches` on
macOS, `%LOCALAPPDATA%` on Windows) instead.

## Building from source

The build needs Ubuntu 24.04, Debian 13, Fedora 40, openSUSE Tumbleweed or a
newer distribution: older releases ship a GTK, FFmpeg, HarfBuzz or meson that
is too old. Before downloading anything, the installer checks the build tools
and every required library, and names whatever is missing or too old.

If the build fails, the installer prints the end of the build output and the
path of the full log, `build-<version>.log` in `~/.cache/nordstjernen-browser`
(`%LOCALAPPDATA%\nordstjernen-browser` on Windows).

Install the build dependencies before you install the package.

Debian/Ubuntu:

```sh
sudo apt install build-essential pkg-config meson ninja-build \
    libgtk-4-dev libepoxy-dev libcurl4-openssl-dev libssl-dev libuchardet-dev \
    libpsl-dev libsqlite3-dev libseccomp-dev libwebp-dev libsdl2-dev \
    libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libswresample-dev
```

Fedora/RHEL (`ffmpeg-devel` comes from RPM Fusion):

```sh
sudo dnf install gcc pkgconf meson ninja-build gtk4-devel libepoxy-devel libcurl-devel \
    openssl-devel uchardet-devel libpsl-devel sqlite-devel \
    libseccomp-devel libwebp-devel SDL2-devel ffmpeg-devel
```

openSUSE (`ffmpeg-devel` comes from Packman):

```sh
sudo zypper install gcc pkgconf meson ninja gtk4-devel libepoxy-devel libcurl-devel \
    libopenssl-devel libuchardet-devel libpsl-devel sqlite3-devel \
    libseccomp-devel libwebp-devel libSDL2-devel ffmpeg-devel
```

macOS on Intel ([Homebrew](https://brew.sh)):

```sh
brew install meson ninja pkg-config gtk4 libepoxy curl openssl@3 \
    uchardet libpsl sqlite webp sdl2 ffmpeg
```

Windows: install [MSYS2](https://www.msys2.org) in `C:\msys64` (or set
`MSYS2_ROOT`), then in its UCRT64 terminal:

```sh
pacman -S --needed tar mingw-w64-ucrt-x86_64-{clang,pkgconf,meson,ninja,gtk4,libepoxy,curl,uchardet,libpsl,sqlite3,libwebp,SDL2,ffmpeg}
```

The browser runs against the MSYS2 libraries it was built with, so keep MSYS2
installed. It is built on your machine and unsigned: if Windows Smart App
Control is on, it blocks the browser, and the Microsoft Store build is the one
to use.

FFmpeg 6.0 or newer is required on Linux and Windows.

## Environment variables

- `NORDSTJERNEN_BUILD_FROM_SOURCE=1` builds from source on Apple silicon too.
- `NORDSTJERNEN_SKIP_INSTALL=1` skips the install step; the browser is then
  installed the first time you run `nordstjernen`. Package managers that do
  not run install scripts (pnpm, `npm --ignore-scripts`) get the same
  behaviour.

## License

Dual-licensed under the Nordstjernen Source License v1.0 or the GNU General
Public License version 3 or later, at your option. Copyright 2026 Andreas
Røsdal.
