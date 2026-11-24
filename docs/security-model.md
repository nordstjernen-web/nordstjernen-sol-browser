# Nordstjernen Sol security model

How Nordstjernen Sol defends against hostile web content. To report a
vulnerability, see [SECURITY.md](../SECURITY.md).

## Threat model

The attacker controls everything fetched: HTML, CSS, JavaScript, images,
fonts, media and PDFs. The user, the kernel and the filesystem outside the
sandbox allow-list are trusted.

**In scope:** memory-safety bugs in our code, including the in-tree forks
of QuickJS (`src/quickjs-bellard/`) and Pango
(`src/web-text-lib/`) and the Rust HTML and URL parsers; Linux sandbox
escapes (Landlock or seccomp); bypasses of the macOS Seatbelt profile or
the Windows process mitigations; same-origin, cookie and cache
partitioning bypasses; HSTS, mixed-content, CSP, iframe-sandbox,
`Cross-Origin-Resource-Policy` and cross-origin isolation bypasses;
URL-bar spoofing.

**Out of scope:** bugs in third-party libraries used unmodified (libcurl,
GTK, GLib, Wuffs, libwebp, libavif, libjxl, PDF.js, FFmpeg, SQLite,
glslang, wgpu-native, OS media decoders, …) — report upstream;
features we do not implement (WebRTC, EME/DRM, JIT, AI web APIs);
Spectre-class side channels; attacks that already
need local code execution as the user. WebGL, experimental WebGPU,
WebAssembly, service workers, `SharedArrayBuffer` under cross-origin
isolation, unpacked extensions, MSE and inline media *are*
implemented and in scope.

## Architecture

Each tab's content is parsed, scripted, laid out and decoded in its own
sandboxed `nordstjernen-renderer` process. The GTK shell parses no page
bytes: it spawns renderers, blits their shared-memory framebuffers and
owns the profile. The renderer still does its own networking and
storage; there is no per-site process and no network broker (see
[Known gaps](#known-gaps)). What a page may do beyond rendering — camera,
WebGL, downloads, popups, pointer lock, fullscreen — is a named capability
(`src/capability.h`) that only the shell grants or revokes; the renderer
asks and waits (`docs/engine-protocol.md`).

## Defenses

### Build

PIE, full RELRO, non-executable stack, separate code segments,
`-fstack-protector-strong`, `-fstack-clash-protection`,
`-fcf-protection=full`, `_FORTIFY_SOURCE=3` (2 as fallback) and
`-Wformat=2 -Wformat-security` (`meson.build`). The browser's own code has
no JIT.

### Privilege drop (`src/security_root.c`)

On Linux and macOS the browser refuses to run as root. On Windows an
elevated launch relaunches itself under the desktop shell's
medium-integrity token and exits; if that is impossible a dialog offers
to quit (default) or continue. `NS_ALLOW_ROOT=1` overrides both.

### Linux sandbox (`src/security.c`)

Renderers and the audio helper set `no_new_privs` and install two
default-deny layers before touching untrusted bytes. Both are mandatory:
a process that cannot enforce Landlock exits with code 78 (so the browser
needs Linux 5.13+ with Landlock enabled), one that cannot load seccomp
exits with 70. The shell runs under a wider Landlock and no seccomp,
because it must spawn renderers and drive the GPU for the window: its
ruleset also makes the renderer's directory executable and leaves
`/dev/shm` writable, for the shared-memory framebuffer where
`memfd_create` is unavailable (`src/gtk/appmain.c`). A seccomp filter for
the shell would have to allow `execve` and `clone`, and the shell parses no
page bytes.

Headless runs (`--headless`, `--dump`, `--eval`, `--export`, `--act`,
`--wpt`, and the npm and pip launchers) execute in the renderer executable
and get the same two-layer sandbox as a tab renderer, through the same
`ns_browser_sandbox()` (`src/headless_main.c`, `src/libnordstjernen.c`):
the config file is read-only and setting changes are not saved, browsing
history, the state directory and Downloads are out of reach,
`$XDG_RUNTIME_DIR` is read-only, and no camera or sound device is granted.
The one addition is the directory of each output file named on the
command line (`--dump=png:`, `pdf:` or `print:`, `--export=pdf:`), which
stays writable. Renderers started by `--webdriver` read the config
read-only as a tab's do.

- **Landlock.** Read-only: system library directories, `/etc`, fonts,
  `/proc`, `/sys`, the display sockets and `$XAUTHORITY`, and of the XDG
  directories only the subdirectories libraries need (fontconfig, Vulkan,
  GStreamer, MIME, dictionaries, shader caches, extensions). Renderers
  read the profile under `~/.config/nordstjernen` and write only its
  `cookies/` directory plus `~/.local/share/nordstjernen` and
  `~/.cache/nordstjernen`. The rest of `$HOME` (`~/.ssh`, `~/.aws`, other
  browsers' profiles, history files) is unreachable; the state directory
  holding the remote-control token is shell-only; no writable directory
  is executable. Camera and sound devices are granted only to a renderer
  started for a site holding the camera permission, and the tab's
  renderer is replaced when that changes. GPU device nodes are writable
  only while WebGL or WebGPU is enabled. On Landlock ABI 6+ signals and
  abstract UNIX sockets are scoped to the sandbox, which keeps a renderer
  away from the session D-Bus.
- **seccomp-bpf.** An allow-list of about 260 syscalls; everything else
  returns `EPERM`. Not allowed: `execve`, `ptrace`, `bpf`, `keyctl`,
  `mount`, `unshare`, `userfaultfd`, `io_uring_*`, `perf_event_open`,
  module and kexec calls, `listen`/`accept`, `tkill`,
  `pidfd_send_signal`. `clone` may not create namespaces, `clone3`
  returns `ENOSYS`, `socket` creates only `AF_INET`, `AF_INET6` and
  `AF_NETLINK` (the routing protocol only), signals and `prlimit64` may
  target only the process itself, `fcntl` cannot set signal ownership, and the `TIOCSTI` /
  `TIOCLINUX` ioctls are refused. With WebGL and WebGPU both off (and
  WebGPU not forced on by `--enable-webgpu` or `NS_WEBGPU_ALLOW=1`),
  `PROT_EXEC` `mprotect`, anonymous executable `mmap` and `memfd_create`
  are refused too (the audio output process keeps `memfd_create`, which
  the sound server's client library needs); with either on they must be allowed, because GPU
  drivers compile shaders to machine code. This raises the cost of
  running injected code, it does not rule it out: a file-backed
  executable `mmap` has to stay allowed for loading libraries, and
  neither seccomp nor Landlock can tell a library from a file the
  renderer wrote into its own cache directory.

`about:nordstjernen` shows the Landlock ABI and whether Landlock, seccomp
and `no_new_privs` are in force in the renderer. `NS_NO_SANDBOX=1`
disables both layers, `NS_NO_SECCOMP=1` only seccomp and
`NS_ALLOW_NO_SECCOMP=1` tolerates a failed load — for debugging only.
Android keeps best-effort behaviour.

### macOS sandbox

The shell and every renderer apply a Seatbelt profile (`sandbox_init`)
before parsing HTML: writes are denied except to the profile, cache and
data directories, Downloads, the temp roots and `/dev`; renderers are
also denied `exec` and AppleEvents. Reads and network stay open, and
there is no syscall filter. If the profile is rejected the process logs a
warning and continues unconfined. The App Sandbox is not used because it
forbids spawning the renderer executables (`docs/macOS.md`).

### Windows process mitigations

`SetProcessMitigationPolicy` at startup, best-effort per policy: forced
image relocation (ASLR), strict handle checks, disabled extension points
(AppInit DLLs, LSPs, IFEO), image-load restrictions (no remote or
low-integrity DLLs, System32 preferred) and, in renderers only, a block
on creating child processes. `ProcessDynamicCodePolicy` is not applied,
because GPU drivers need executable memory. `NS_NO_WIN32_MITIGATIONS=1`
disables the set.

### JavaScript

QuickJS is an interpreter (the fork of Bellard's engine in
`src/quickjs-bellard/`). Every
top-level navigation gets a fresh runtime, so nothing from the previous
document's JavaScript survives into the next. DOM wrappers are
invalidated when their node is freed.

**Heap cage.** Each renderer's JavaScript heap is one reserved 4 GB range
with guard regions on both sides, separate from the DOM, layout and
network memory. Values hold 32-bit offsets into it, ArrayBuffer and
typed-array pointers are masked into it, host pointers and native
function pointers live in tables outside it, and the allocator keeps its
bookkeeping outside and aborts on bad frees. This raises the cost of
exploiting a JavaScript heap bug; it is not yet a boundary of its own
(`docs/heap-hardening.md`).

**Frames.** A frame gets its own realm in the tab's runtime. A
cross-origin or opaque-origin frame's realm is built from scratch, so it
shares no constructors or prototypes with its embedder, and it sees its
parent only through a cross-origin proxy. The `sandbox` attribute is
enforced (scripts, same-origin, forms, modals, popups, top navigation,
downloads) and inherited by nested frames. `postMessage` checks the
target origin. A call made through a cross-origin window proxy
(`postMessage`, `location.href`, `location.replace`, `close`, `focus`,
`blur`) runs a native of the target document; an error it throws is
re-created in the caller's realm with only its name and message, so the
caller never holds an object of the target's realm.

### GPU (`src/glsl_validate.c`, `src/webgpu.c`)

Every WebGL shader is checked against the WebGL rules and compiled by
glslang before the driver sees it, and every draw is checked against the
vertex, index and uniform buffer ranges it reads. Storage a page can read
back (`bufferData(size)`, `texStorage2D/3D`, renderbuffers, the drawing
buffer) starts as zeros. The GL context is created
with robust buffer access where the driver offers it, and on Linux and the
BSDs not at all otherwise; on Windows an ordinary context is used when the
driver lacks the extension. WebGPU shaders are validated by naga in
wgpu-native, which adds bounds checks; only the Vulkan, Metal and D3D12
backends are enabled. Both APIs can be switched off in Settings.

### Network (`src/net.c`)

TLS verification is always on with TLS 1.2 as the minimum; only `http`,
`https` (and top-level `ftp`) are fetched; an HTTPS request never
redirects to HTTP; redirects and response sizes are capped. HSTS is
enforced, but stored only from top-level or same-site responses, so it
cannot be used as a third-party supercookie. Active mixed content is
blocked and passive mixed content upgraded. Private Network Access stops
a page from reaching a more private address space than its own, which
also defeats DNS rebinding. A certificate error can be overridden (off by
default) only for a top-level, non-HSTS navigation, which then carries no
cookies; the resulting document has an opaque origin, without access to
the site's cookies or storage. Once the setting is on, the override is
applied automatically to every such navigation, with no per-site prompt.

A navigation to a URL flagged by Safe Browsing stops at a warning page.
Safe Browsing is on by default. Google's hash-prefix lists are kept on the
device, and only when an address matches one is a 4-byte prefix of its
hash sent to Google, never the address itself.

### Origins, cookies and storage

- Cookies and the HTTP cache are partitioned per top-level site (via the
  Public Suffix List); third-party cookies are blocked by default.
  Cookies live in a browser-owned SQLite store (`src/cookies.c`); the
  engine builds every `Cookie` header itself and keeps `HttpOnly`
  cookies out of `document.cookie`.
- `file:` documents are opaque origins: a local page cannot read other
  local files.
- Only `https:` and real loopback hosts are trustworthy origins.
  `ns_host_is_loopback` (`src/netutil.c`) parses the address: `localhost`,
  `*.localhost`, `127.0.0.0/8` and `::1` qualify, a name that merely
  starts with `127.` does not. The same check decides service-worker
  registration and the address bar's "Not secure" chip.
- `Cross-Origin-Resource-Policy` is honoured on every page. A top-level
  document served from a trustworthy origin with
  `Cross-Origin-Opener-Policy: same-origin` and
  `Cross-Origin-Embedder-Policy: require-corp` or `credentialless` is
  cross-origin isolated: only then is `SharedArrayBuffer` exposed, in the
  page and the dedicated workers it starts, and only then may a shared
  buffer or a shared `WebAssembly.Memory` be posted to a worker. Its
  cross-origin `no-cors` subresources must opt in with
  `Cross-Origin-Resource-Policy`.
- CSP is enforced for fetch directives, `base-uri`, `form-action`,
  `frame-ancestors`, `worker-src` and `upgrade-insecure-requests`,
  including nonces, hashes and `'unsafe-eval'` / `'wasm-unsafe-eval'`.
  Subresource Integrity is checked before scripts and stylesheets apply.
- A document's `Referrer-Policy` header and `<meta name="referrer">`
  limit the `Referer` its requests carry; the stricter of that policy and
  the browser setting applies.
- The URL bar shows hosts in Punycode, so homographs are visible.
- Profile directories are `0700` and files `0600` (ACL-restricted on
  Windows). Cache file names are SHA-256 hashes.

### Parsers and media

HTML and URLs are parsed by the in-tree parsers in safe Rust (`src/html5_rs/`,
`src/url_rs/`), charsets detected by uchardet. Images
are decoded by Wuffs, libwebp and (optionally) libavif and libjxl; SVG is
in-engine; PDFs are rendered by PDF.js, as sandboxed script. Video is decoded in the
renderer by pl_mpeg, FFmpeg (WebM, MP4, VP9/VP8, AV1 and others) or the
operating system's decoders (H.264, AAC, hardware VP9/AV1/HEVC). Audio is
decoded in the `nordstjernen-audio` helper, which runs under the same
Linux sandbox and is split in two: the decoding process holds no socket
and writes PCM into a pipe that a second process copies to the sound
device. The helper only plays files the renderer has already fetched
through the normal network path. Unsupported media shows a poster; the
shell never launches an external player.

### Downloads

The renderer streams a download to a `.part` file in its cache (capped at
16 GiB); the shell only moves the finished file into Downloads and marks
its origin (quarantine attribute, `user.xdg.origin.url` or
`Zone.Identifier`). Executable and installer types need the user's
confirmation.

### Automation servers

Both bind loopback only.

- `--webdriver` (`src/gtk/webdriver.c`) listens only when the browser is
  started with that flag. It rejects requests with a
  non-loopback `Host`, any `Origin` header, or a content type a web page
  could send without a CORS preflight. There is no further
  authentication: anything local that reaches the port controls the
  browser.
- `--remote-control` (`src/gtk/remotectl.c`) requires a random token
  stored in the state directory, which renderers cannot read. It listens
  when started with its flag, and also whenever the `remote_control`
  setting is on: switched on in Settings (after the confirmation dialog)
  or written to the config file while the browser runs.

## Known gaps

Findings of the latest source review that are still open are listed in
[security-review-report-2026-11-21.md](security-review-report-2026-11-21.md).

- Frames are a realm boundary, not a process boundary: a memory-safety
  bug in one frame is not contained from the rest of its tab.
- The process boundary is per tab, not per site, and every renderer
  fetches for itself and opens the profile's cookie, cache and storage
  databases. A compromised renderer therefore holds the credentials of
  every site, not only the one it shows. Moving networking, cookies, cache
  and storage behind the shell is not built.
- The shell has no seccomp filter (see "Linux sandbox" above).
- Extensions have no permission model (content scripts are injected by
  `matches` alone) and run content scripts in the page's JavaScript
  world.
- The settings page is rendered by the renderer. The shell writes the
  config and shows every non-cosmetic change in a confirmation dialog,
  but that dialog is the only barrier against a compromised renderer.
- The shell loads its built-in history page for any `about:history`
  navigation, including one a renderer asks for.
- Under `--headless` a document loaded through the certificate override
  keeps the site's origin; only the windowed browser gives it an opaque
  one.
- The `referrerpolicy` attribute, `rel=noreferrer` and the
  `referrerPolicy` option of `fetch()` do not yet limit the `Referer`
  header.
- FFmpeg in the renderer probes with every demuxer the system library
  ships, not an allow-list.
- On kernels without `LANDLOCK_RESTRICT_SELF_TSYNC`, threads that
  libraries started before the sandbox was applied are confined by
  seccomp only.
- Windows has no filesystem sandbox (no AppContainer or low-integrity
  token), and the macOS and Windows audio helper is not syscall-confined.
