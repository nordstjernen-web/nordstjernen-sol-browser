# nordstjernen-browser

[Nordstjernen Web Navigator](https://nordstjernen.org) is a minimal, secure
web browser written from scratch in C with GTK 4. It does not phone home.

```sh
npm install -g nordstjernen-browser
nordstjernen https://example.com
```

## What installing does

| System | What happens |
| --- | --- |
| Linux, BSD, Intel macOS | Nordstjernen is built from the release source on your machine with meson and ninja, against your system's GTK 4, FFmpeg and other libraries. This takes several minutes. |
| macOS on Apple silicon | The Developer ID signed and notarised release build is downloaded from GitHub and checked against the release's `SHA256SUMS`. |
| Windows | Nordstjernen is distributed through the [Microsoft Store](https://apps.microsoft.com/detail/9NW8T7W5Z4PL); the `nordstjernen` command opens its Store page. |

The browser is installed inside the npm package, so `npm uninstall -g
nordstjernen-browser` removes it. If that directory is not writable, the
browser goes into `~/.cache/nordstjernen-browser` (`~/Library/Caches` on
macOS) instead.

## Building from source

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

FFmpeg 6.0 or newer is required.

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
