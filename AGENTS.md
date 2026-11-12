# Nordstjernen — Codex operating guide

Nordstjernen ("Nordstjernen Web Navigator") is a web
browser written from scratch in **C**, using **GTK 4** for the UI and
**libcurl** for networking. Targets Linux, macOS, and Windows.

See `README.md` for the product vision. Nordstjernen is a fresh
implementation — there is no upstream browser engine, no fork,
nothing imported.

## Design constraints

- Minimalistic, compact, secure. Source should be readable and
  maintainable by a single human.
- HTML5 + modern CSS + modern JavaScript, supported pragmatically as
  far as is feasible without bloat.
- **No** AI-style web APIs. WebGL **is** supported: a working,
  minimalist WebGL 1 / 2 over OpenGL ES (`src/webgl.c`, see
  `docs/webgl.md`). It is enabled by default, can be disabled globally in
  Settings, and reports active use in the browser status bar.
- **WebGPU** (`navigator.gpu`) is an **experimental** feature that
  layers `src/webgpu.c` over the external wgpu-native library. The
  `webgpu` meson feature is `auto`: required on glibc Linux, macOS
  and Windows (fetched by a meson wrap when not found), optional
  elsewhere. When built it is on at runtime by default, like WebGL;
  Settings can turn it off, and `--enable-webgpu` (which sets
  `NS_WEBGPU_ALLOW=1`) overrides that. See `docs/webgpu.md`.
- The one vendored, in-tree video codec is MPEG-1, decoded by the
  vendored pl_mpeg decoder (`subprojects/plmpeg/`, wrapped by
  `src/video_decode.c`); such `<video>` plays inline (`src/video.c`).
  Audio plays via the unsandboxed `nordstjernen-audio` helper
  (`src/audio/main.c`), which decodes MPEG-1/MP2 (pl_mpeg) and MP3
  (vendored minimp3, `src/audio/minimp3.h`) and outputs through SDL2.
  MSE video frames decode in the sandboxed renderer, which keeps the
  appended segments in memory and plays the buffered range around the
  playhead (see `docs/media.md`).
  When FFmpeg's libav\* system packages are present, the build also
  gains inline WebM (VP9/VP8 video + Opus/Vorbis audio) — never
  vendored. H.264 and AAC are decoded by the operating system (Media
  Foundation, VideoToolbox/AudioToolbox, MediaCodec, or GStreamer on
  Linux; `src/media_os*.c`), never by a bundled decoder.
  Other `<audio>`/`<video>` codecs render a poster and
  play overlay; the renderer resolves the media URL under the
  pointer via `ns_browser_media_at`.
- UI strings are English-source and translated to the operating-system
  language at startup through the in-tree catalogue lookup (`src/i18n.c`,
  `data/i18n/*.lang`); English is the fallback for any string a catalogue
  does not cover. No gettext dependency.
- Does not phone home, does not telemeter the user.

## Comments policy

**The code is self-explaining. Don't write code comments.**

- Each source file gets exactly one short header comment at the top
  naming the file and (at most) one sentence on what it does. That's
  it.
- No inline `/* … */` or `//` comments inside functions, in struct
  declarations, around tricky branches, or anywhere else. Rename a
  variable or extract a function instead.
- No "section banner" comments (`/* ---------- helpers ---------- */`).
  Group code by file or function instead.
- No `TODO`/`FIXME`/`XXX` markers — file a real task instead.
- Bundled external projects are exempt from this comments policy.
  In particular, do not reformat or strip comments from
  `src/quickjs/`, or `subprojects/` just to satisfy Nordstjernen style.

## Autonomous mode — read this every session

This repo is driven by Codex in long uninterrupted sessions.
**Default to acting, not asking.**

- **Permissions: run with sandboxing/approvals relaxed for this repo.**
  `.claude/settings.json` sets `defaultMode: bypassPermissions` plus a broad
  allow-list for the build/run/git/inspect workflow (used by Claude Code; the
  equivalent for other harnesses is full-access / never-ask). Routine
  build/run/git/inspect commands must never prompt. Don't burn turns getting
  individual commands approved.
- **Don't ask "do you want me to proceed?", "should I continue?",
  "ready to commit?"** — just do it. The user interrupts if they
  disagree.
- **Don't summarize after every step.** One-line status is enough.
- **Don't pause for path/file/branch confirmation when context is
  unambiguous.** Grep, pick, proceed.
- **Commit and push aggressively.** Small commits, push to
  `origin/main` as soon as a logical unit lands.
- **Run for hours.** Diagnose, fix, retry. Only stop on genuine
  external blockers. When stopping: one line on what's blocked.
- **Never ask the user to run the build.** Run it yourself.
- **Local machine is the build *and* run oracle.** The repo can be
  driven from either a Linux box (GTK 4 / libcurl / meson / clang +
  an X session at `DISPLAY=:0`) or a Windows 11 box via MSYS2
  UCRT64 (same toolchain, same meson/ninja invocation; the binary
  is `./builddir/src/gtk/nordstjernen.exe`). Every commit must pass
  `meson compile -C builddir` locally before pushing. Smoke-launch
  the browser (in the background, then kill it) on material changes
  — that's the per-change correctness gate, not CI. See
  `docs/Windows.md` for the MSYS2 setup; the rest of this guide
  uses Unix-style invocations that work in either shell.
- **CI is enabled.** The Linux / macOS / Windows / musl / Java
  workflows run on every push to `main` and every PR targeting
  `main`, plus manual `workflow_dispatch`. Local Linux is still the primary
  correctness gate before pushing; CI provides cross-platform
  sanity coverage.

## Build / verify locally

The intended build system is **meson + ninja**. From a clean checkout:

```sh
meson setup builddir
meson compile -C builddir
./builddir/src/gtk/nordstjernen
```

Two QuickJS engines are integrated into the main tree, and the `quickjs`
meson option picks one (`docs/quickjs.md`):

- `quickjs` (default): `src/quickjs-bellard/`, a fork of Fabrice Bellard's
  [QuickJS](https://github.com/bellard/quickjs). It carries the heap
  hardening and the browser hooks, and upstream's `Intl` and `Temporal`.
- `quickjs-ng`: `src/quickjs/`, a fork of
  [quickjs-ng](https://github.com/quickjs-ng/quickjs) with the same
  hardening and hooks and its own `Intl`/`Temporal`.

We modify both freely. Neither is a meson subproject: each directory's
`meson.build` is loaded via `subdir()` from the top-level and exposes
`libquickjs` as a declared dependency directly in the parent scope.

### Two engines, one binding

- **Engine code includes `"ns_quickjs.h"`, never `<quickjs.h>`.** The
  binding is written against the quickjs-ng API. On Bellard's engine
  (`NS_QUICKJS_ORIGINAL`) the engine exports the browser hooks itself and
  `src/ns_quickjs.h` / `src/ns_quickjs.c` supply the rest.
- **A new quickjs-ng-only call goes into Bellard's engine or the adapter**,
  not behind an `#ifdef` at the call site: the default build fails to
  compile until it exists.
- CI builds the default only: build `-Dquickjs=quickjs-ng` locally after
  binding changes (`docs/quickjs.md`, "Checking a change").

### HTML engine: in-tree Rust HTML5 parser

The single HTML→DOM backend is the project's own HTML5 parser,
`src/html5_rs/`: a WHATWG tokenizer and tree builder in safe Rust
(`#![forbid(unsafe_code)]`, no crates), shared with
[Southstar](https://github.com/nordstjernen-web/southstar-browser)
(`rust/html5` there). It builds through a `TreeSink` trait;
`src/parsers_rs/lib.rs` is the C ABI (`src/parsers_rs.h`) and its sink
calls the `ns_h5_*` functions in `src/html_parser.c`, which create
`ns_node`s directly — there is no intermediate tree. Node strings live in
one arena per document, attached as the root's `backing`. The entity table
(`entities.rs`) is generated by `scripts/gen-html-entities.py`. Rust
(`rustc` 1.91 or newer, edition 2024) is therefore a **required**
toolchain; meson builds the crates with plain `rustc`, no Cargo. Keep
`src/html5_rs/` and `src/url_rs/` in step with Southstar's copies rather
than letting them drift.

### Image decoding: Wuffs

PNG/APNG, GIF, BMP, and JPEG bytes are decoded through
[Wuffs](https://github.com/google/wuffs), a memory-safe
transpiled-to-C image-decoder library. The single-file release is
vendored at `subprojects/wuffs/wuffs-v0.4.c` and built as a static
subproject. `src/image.c::ns_image_decode_bytes` runs the decoders
in order: ICO (`src/image_ico.c`) → Wuffs (PNG/APNG, GIF, BMP, JPEG)
→ WebP via libwebp (`src/image_webp.c`) → AVIF via libavif when built with it
(`src/image_avif.c`, optional) → SVG in-engine (`src/svg.c`). Nothing
follows: gdk-pixbuf no longer decodes page images, so an unsupported
format fails to decode.

### URL parsing: in-tree Rust URL parser

The `ns_url_*` helpers in `src/net.c` route URL resolution, the URL API
setters, origin extraction and host extraction through `src/url_rs/`, the
project's own WHATWG URL parser in safe Rust (host parser, UTS #46 IDNA,
Punycode, NFC; shared with Southstar's `rust/url`), over the `ns_rs_url_*`
functions of `src/parsers_rs.h`. Its Unicode tables (`tables.rs`) are
generated by `scripts/gen-url-tables.py`. No separate URL library or
build option.

### Charset detection: uchardet

Required dependency (Debian/Ubuntu `libuchardet-dev`,
Fedora/RHEL `uchardet-devel`). `ns_html_decode_body` hands the
response body to [uchardet](https://www.freedesktop.org/wiki/Software/uchardet/)
to identify the charset, then `g_convert`s to UTF-8. No
hand-rolled BOM / HTTP-charset / meta-charset sniffing — uchardet
handles all of that internally. The Latin-1 fallback only fires
if uchardet can't classify the bytes at all.

### WebP: libwebp

Required dependency (Debian/Ubuntu `libwebp-dev`, Fedora/RHEL
`libwebp-devel`, openSUSE `libwebp-devel`, Alpine `libwebp-dev`,
MSYS2 `mingw-w64-ucrt-x86_64-libwebp`, Homebrew `webp`). WebP —
lossy VP8 (the dominant variant served by the BBC, Wikipedia
thumbnails, and most modern CDNs), lossless VP8L, and **animated
WebP** (via libwebpdemux's `WebPAnimDecoder`, playing like animated
GIFs) — is decoded in-tree by `src/image_webp.c`, tried right after
the Wuffs decoders. No gdk-pixbuf loader, no
`loaders.cache` registration, and no sandbox interaction is
involved; the old `webp-pixbuf-loader` runtime dependency is gone.

### Web Cryptography: OpenSSL libcrypto

Required dependency (Debian/Ubuntu `libssl-dev`, Fedora/RHEL
`openssl-devel`, openSUSE `libopenssl-devel`). `crypto.subtle` (the
WebCrypto SubtleCrypto surface) is implemented in `src/webcrypto.c`
directly over OpenSSL's EVP/`OSSL_PARAM` APIs — hashing, HMAC, AES
(GCM/CBC/CTR/KW), RSA (PKCS1/PSS/OAEP), ECDSA/ECDH (P-256/384/521),
Ed25519/X25519, PBKDF2 and HKDF. The QuickJS `CryptoKey` class and `subtle.*`
argument marshalling live in `src/js.c`. OpenSSL is already linked
transitively through libcurl's TLS backend on Linux and
Windows/MSYS2; `meson` depends on `libcrypto` explicitly so the
headers resolve.

### WebAssembly: WAMR

The `WebAssembly` JS API (`compile`, `instantiate`, `Memory`,
`Table`, `Global`, externref) is implemented in `src/wasm.c` over a
vendored subset of the [WebAssembly Micro Runtime
(WAMR)](https://github.com/bytecodealliance/wasm-micro-runtime)
interpreter at `src/wamr/`. It runs wasm-bindgen bundles. See
`docs/webassembly.md`.

System packages required on Debian/Ubuntu:

```sh
sudo apt install build-essential pkg-config meson ninja-build rustc \
    libgtk-4-dev libepoxy-dev libcurl4-openssl-dev libssl-dev libuchardet-dev \
    libpsl-dev libsqlite3-dev libseccomp-dev libwebp-dev libsdl2-dev glslang-dev \
    libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libswresample-dev
```

On Fedora/RHEL:

```sh
sudo dnf install gcc rust pkgconf meson ninja-build gtk4-devel libepoxy-devel libcurl-devel \
    openssl-devel uchardet-devel libpsl-devel sqlite-devel \
    libseccomp-devel libwebp-devel SDL2-devel glslang-devel ffmpeg-devel
```

(`ffmpeg-devel` comes from RPM Fusion.)

On openSUSE:

```sh
sudo zypper install gcc rust pkgconf meson ninja gtk4-devel libepoxy-devel libcurl-devel \
    libopenssl-devel libuchardet-devel libpsl-devel sqlite3-devel \
    libseccomp-devel libwebp-devel sdl2-compat-devel glslang-devel ffmpeg-devel
```

(`ffmpeg-devel` comes from Packman.)

`libseccomp` is required on Linux — `meson setup` fails without it.
On macOS and Windows it is not used and the syscall filter is a no-op.

The FFmpeg libav\* packages are required on Linux and Windows too (MSYS2
`mingw-w64-ucrt-x86_64-ffmpeg`, or the LGPL build from
`scripts/build-ffmpeg-lgpl.sh`): they carry inline WebM playback, and
`meson setup` fails without them or with a pre-6.0 FFmpeg. On macOS they
are auto-detected, and a build without them carries no libav dependency and
shows the poster and play overlay for WebM instead of playing it inline.

Optional: `libenchant-2-dev` (plus a dictionary such as `hunspell-en-us`)
enables on-screen spell-checking; `libavif-dev` adds AVIF decoding and can be
dropped with `-Davif=disabled`. Both are auto-detected.

### Fast iteration (recommended for AI/Codex loops)

`ccache` is the single biggest build-time win and meson picks it up
automatically. With `ccache` installed, a clean `meson setup builddir
&& meson compile -C builddir` drops from ~35s to ~1s once the cache
is warm — in-tree libraries (quickjs) hit the cache
and re-link in negligible time. Install once:

```sh
sudo apt install ccache       # Debian/Ubuntu
sudo dnf install ccache       # Fedora/RHEL
```

Optionally use the `lld` linker for faster final links
(`CC_LD=lld meson setup builddir`). Not required.

`./scripts/dev.sh build` runs `meson setup` (only if needed) and
`meson compile -C builddir` in one shot — use it instead of typing
the two commands separately.

## Security fixes across editions

[Northstar](https://github.com/nordstjernen-web/northstar-browser) and
[Southstar](https://github.com/nordstjernen-web/southstar-browser) share
most of this engine, so a memory-safety or denial-of-service fix here
usually applies there as well. Give every such commit a trailer naming
the bug, and name the source commit when porting one:

```
Security-Fix: css-grid-auto-repeat-overread
Ported-From: nordstjernen-web/northstar-browser@<full sha>
```

Put both lines in the final trailer block, next to `Co-Authored-By`,
and keep the slug when porting so the same bug carries the same name in
every tree. `git log --format=%B | sed -n 's/^Security-Fix: //p' | sort -u`
lists a tree's fixes; a slug that one edition has and another lacks is
a fix still to check there. Port only commits whose author the
destination's provenance rules allow (Northstar's `AGENTS.md`).

## Definition of done

A change is done when:

1. It compiles cleanly (no new warnings) with the configured GCC and
   Clang flags.
2. The browser launches and the affected UI path works manually.
3. The change is committed and pushed to `origin/main`.

Note: this project has **no automated test suite** and no plans to
add one. Verify behavior by running the browser. Don't add unit /
integration / property / fuzz tests, don't add a `tests/` directory,
don't add `meson test` targets.

## Don't

- Don't introduce Mozilla/Gecko code, WebKit code, or any other
  upstream browser engine source. Nordstjernen is an independent
  implementation, not a fork. The one
  exception is the PDF viewer: PDF.js (Mozilla, Apache-2.0) is not engine
  code; it runs as ordinary sandboxed script on the page that shows a PDF
  (`src/pdf.c`, `data/js/pdf-viewer.js`). It is fetched at build time
  through `subprojects/pdfjs-dist.wrap` and embedded, never committed;
  `-Dpdfjs=disabled` drops it, and PDFs then show a notice with a link to
  the file. There is no native PDF library in the build.
- Don't add AI-style web-API surface area, even as stubs. (WebGL
  already exists as a deliberate exception — extend
  `src/webgl.c`, don't re-architect it. WebGPU is an experimental
  exception layered over external wgpu-native — `src/webgpu.c`, kept
  switchable in Settings (`webgpu_enabled`); don't vendor its
  library.)
- Don't add telemetry, crash reporters, update pingers, or "studies"
  infrastructure. UI translation goes through `src/i18n.c` and the
  `data/i18n/*.lang` catalogues — don't introduce gettext or `.po`
  tooling.
- Don't write planning docs unless asked.
