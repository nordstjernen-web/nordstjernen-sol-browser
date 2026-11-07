# Nordstjernen for iOS

An iOS host app that drives the Nordstjernen browser engine through its C
embedding API (`src/libnordstjernen.h`). The engine — HTML parsing (lexbor),
CSS cascade + layout, JavaScript (QuickJS), image decoding (Wuffs) and cairo
painting — is the same code used on the desktop and Android. iOS is
architecturally an **Android-style port, not a macOS one**: the macOS build
ships the full GTK 4 desktop app, but GTK 4 does not run on iOS, so iOS drives
the GTK-free engine (`libnordstjernen`) from a thin UIKit/Swift shell the same
way Android drives it from a Kotlin/JNI shell.

## Architecture

```
 UIKit/Swift UI  (URL bar, toolbar, scrolling PageView)
        │  C embedding API (src/libnordstjernen.h + ios/App/Bridge/ns_ios.h)
 libnordstjernen  (engine: net, dom, css, layout, js, paint)
        │
        └─► glib · gobject · gio · cairo · pango · pangocairo · harfbuzz ·
            freetype · fontconfig · libcurl · sqlite3 · uchardet · libpsl
            (cross-compiled for iOS, static)
```

As on Android, iOS drops **GTK 4** (`meson.build`'s
`is_mobile`): `GdkTexture` is replaced by the `ns_texture` abstraction
(`src/texture.c`), the SVG / fallback image decoders are gated out, and
`ns_browser_render_argb32()` paints a viewport region straight into a buffer the
`PageView` wraps as a `CGImage`. The engine's Darwin paths (`getentropy`,
`_NSGetExecutablePath`, `sys/xattr.h`) are the same on iOS as on macOS; the one
macOS-desktop-only path, the Seatbelt sandbox, is guarded off for iOS
(`TARGET_OS_IPHONE`).

## Layout

```
ios/
  App/
    project.yml                 XcodeGen project (app target, iOS 15+)
    Sources/
      AppDelegate.swift         window + root view controller
      BrowserViewController.swift  URL bar, toolbar, history
      PageView.swift            scrolling render surface (engine → CGImage)
      BrowserEngine.swift       Swift wrapper over the C embedding API
    Bridge/
      ns_ios.h / ns_ios.c       C bridge: engine init (data dir + CA bundle)
      Nordstjernen-Bridging-Header.h
    Resources/
      Info.plist
    vendor/                     staged by build-engine.sh (git-ignored)
  scripts/
    check-ios-sources.sh        cross-check engine sources against the iOS SDK
    build-engine.sh             cross-compile libnordstjernen.a + write xcconfig
    fetch-prebuilt-deps.sh      download the prebuilt iOS dependency sysroot
```

## Building the app

```sh
# 1. Fetch the prebuilt iOS dependency sysroot (device + simulator):
export NORDSTJERNEN_IOS_SYSROOT="$HOME/.cache/nordstjernen-ios-sysroot"
ios/scripts/fetch-prebuilt-deps.sh --sysroot "$NORDSTJERNEN_IOS_SYSROOT"

# 2. Cross-compile the engine against it (writes ios/App/vendor + the xcconfig):
ios/scripts/build-engine.sh device
ios/scripts/build-engine.sh simulator

# 3. Generate and build the Xcode project:
cd ios/App
xcodegen generate
xcodebuild -project Nordstjernen.xcodeproj -scheme Nordstjernen \
    -sdk iphonesimulator -configuration Release CODE_SIGNING_ALLOWED=NO build
```

Requires macOS with Xcode, `meson`/`ninja`, and `xcodegen`. On-device builds
additionally need a signing identity + provisioning profile.

The **dependency sysroot** (glib/cairo/pango/curl/sqlite/… cross-compiled for
iOS) is produced and published by the `nordstjernen-dependencies-build` repo's
`build-ios-deps` workflow (`ios-sysroot-latest` release, plus an immutable
`ios-sysroot-<date>-<sha>` release per commit; `fetch-prebuilt-deps.sh` verifies
the checksums and, with `gh` installed, the build-provenance attestation),
mirroring how the Android sysroot is produced. Building that sysroot is the bulk of the porting
work; `build-engine.sh` and the app consume it.

## CI

iOS is not built in CI; the former `ios.yml` workflow was removed. On a Mac,
`ios/scripts/check-ios-sources.sh` cross-checks every engine translation unit
against the iOS SDK (device and simulator), and `fetch-prebuilt-deps.sh`,
`build-engine.sh` and `xcodebuild` assemble the unsigned simulator app.

## Status

* **Built end-to-end on macOS (no longer in CI):** the published dependency
  sysroot is fetched, the engine cross-compiles to `libnordstjernen.a` for
  device and simulator, and `xcodebuild` assembles the unsigned UIKit app for
  the iOS Simulator.
* **Done & verified on Linux:** the engine builds GTK-free for the iOS
  configuration — `meson.build`'s `is_mobile` predicate produces an
  engine-library-only build (no GTK shell, renderer, audio/video helpers),
  confirmed by configuring `-Dios=true` and by a clean desktop build after the
  refactor. The macOS-Seatbelt-sandbox `TARGET_OS_IPHONE` guard is in place.
* **Manual:** the engine iOS-portability check (device + simulator),
  `ios/scripts/check-ios-sources.sh`.
* **Not yet done:** a signed on-device build (needs a signing identity +
  provisioning profile); booting and driving the app in a simulator and
  real-world browsing validation; App Store
  submission.
