# Nordstjernen on iOS — build & architecture

The iOS port lives in [`ios/`](../ios). This document is the engineering guide:
how the port is structured, how to build it, and what remains. For the app
layout and a status snapshot see [`ios/README.md`](../ios/README.md).

## The port in brief

- **UIKit shell in Swift** (`ios/App/Sources/`) over the C embedding API
  (`src/libnordstjernen.h`, see [`Embedding.md`](Embedding.md)) through a
  thin C bridge (`ios/App/Bridge/ns_ios.*`) — URL bar, back/forward
  history, reload, a scrolling `PageView`, and tap-to-follow-link.
- The **same engine**, cross-compiled to a static
  `libnordstjernen.a`. Like Android, iOS drops GTK 4,
  so its only native dependencies are the GLib/cairo/pango stack plus
  libcurl/sqlite3/uchardet/libpsl — all plain C, no Rust.
- **Targets:** iOS 15+, `arm64` device and `arm64` simulator. No bitcode.

## Why iOS is an Android-style port, not a macOS one

The macOS build ships the full **GTK 4 desktop app**. GTK 4 has no iOS backend
(iOS renders through UIKit/Core Animation, not X11 or Wayland), so "builds on
macOS" does not carry to iOS. What *is* portable is the engine underneath the
toolkit — the same `libnordstjernen` that Android already runs headless. iOS
therefore reuses the Android split: the GTK-free engine as a library, plus a
native shell that renders its ARGB output and forwards input.

In `meson.build` this is the `is_mobile = is_android or is_ios` predicate. An
iOS configure (`-Dios=true`, passed by `ios/scripts/build-engine.sh` alongside
its generated cross file) builds only the
embeddable engine library — no desktop shell, out-of-process renderer, or
audio/video helper executables — exactly as Android does.

## Rendering

`ns_browser_render_argb32()` paints a viewport region into a caller buffer in
Cairo-native ARGB32 (premultiplied, host byte order). On little-endian arm64
that is exactly a Core Graphics `byteOrder32Little` + `premultipliedFirst`
image, so `PageView` wraps the buffer as a `CGImage` with no channel swizzle.
The engine lays the page out at a phone-width CSS viewport and renders scaled by
the display density (the screen's `scale`), so text is crisp at native resolution —
re-rendered by the engine on scroll rather than bitmap-stretched.

## Building

The engine is not compiled inside Xcode; a script cross-compiles it against a
prebuilt dependency sysroot and writes the static libraries plus a generated
`xcconfig` that the Xcode project links against.

```sh
# 1. Fetch the prebuilt iOS dependency sysroot (device + simulator):
export NORDSTJERNEN_IOS_SYSROOT="$HOME/.cache/nordstjernen-ios-sysroot"
ios/scripts/fetch-prebuilt-deps.sh --sysroot "$NORDSTJERNEN_IOS_SYSROOT"

# 2. Cross-compile the engine (stages ios/App/vendor/<platform>/ and writes
#    ios/App/vendor/nordstjernen.xcconfig):
ios/scripts/build-engine.sh device
ios/scripts/build-engine.sh simulator

# 3. Generate and build the Xcode project:
cd ios/App
xcodegen generate
xcodebuild -project Nordstjernen.xcodeproj -scheme Nordstjernen \
    -sdk iphonesimulator -configuration Release CODE_SIGNING_ALLOWED=NO build
```

Requires macOS + Xcode, `meson`/`ninja`, and `xcodegen`. On-device builds also
need a signing identity and provisioning profile.

## The dependency sysroot

The bulk of the porting work is cross-compiling glib, gobject, gio, gmodule,
cairo, pango, pangocairo (and their transitive deps: harfbuzz, freetype,
fontconfig, pixman, fribidi, libffi, pcre2, expat, zlib, libpng), libcurl (with
openssl, nghttp2, brotli), sqlite3, uchardet, libpsl and libwebp for iOS —
`arm64` device and `arm64` simulator, static.

This mirrors the Android sysroot exactly, and is produced the same way: the
[`nordstjernen-dependencies-build`](https://github.com/nordstjernen-web/nordstjernen-dependencies-build)
repo's `build-ios-deps` workflow cross-compiles the whole stack on a macOS
runner and publishes it as the rolling `ios-sysroot-latest` release plus an
immutable per-commit `ios-sysroot-<date>-<sha>-r<run>` release.
[`ios/sysroot.lock`](../ios/sysroot.lock) pins the immutable tag and the sha256
of its `SHA256SUMS`; `fetch-prebuilt-deps.sh` follows the lock, refuses a
`SHA256SUMS` that hashes differently, and verifies each tarball's GitHub
build-provenance attestation when an authenticated `gh` is available
(`NORDSTJERNEN_REQUIRE_ATTESTATION=1` makes that mandatory). `--tag
ios-sysroot-latest` bypasses the lock. The `sysroot-bump` workflow opens a pull
request when a newer attested release exists.

## CI

iOS is not built in CI. The former `ios.yml` workflow (the engine iOS source
check and the UIKit simulator app build) was removed; run the same steps by hand
on a Mac when working on the port: `ios/scripts/check-ios-sources.sh` checks
every engine translation unit against the iOS SDK, and `fetch-prebuilt-deps.sh`
plus `build-engine.sh` and `xcodebuild` assemble the simulator app.

## Status

- **Built end-to-end on macOS (no longer in CI):** the published iOS
  dependency sysroot is fetched and relocated, the engine cross-compiles to
  `libnordstjernen.a` for both device and simulator, and `xcodebuild` assembles
  the unsigned UIKit app for the iOS Simulator.
- **Verified (Linux):** the GTK-free `is_mobile` engine build (engine library
  only under `-Dios=true`); the desktop build is unchanged by the refactor; the
  `TARGET_OS_IPHONE` sandbox guard.
- **Not yet done:** a signed on-device build (needs a signing identity and a
  provisioning profile); booting and driving the app in a simulator, and
  real-world browsing validation; App Store submission.

## Distribution (future)

Like the desktop and Android builds: free, ad-free, no telemetry, no in-app
purchase. App Store submission (signing, provisioning, review) is out of scope
until the app builds and runs on device.
