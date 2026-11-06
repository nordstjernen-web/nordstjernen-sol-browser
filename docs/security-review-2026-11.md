# Security review, November 2026

A review of Nordstjernen's own source for security vulnerabilities, made at
`8b89368` (1.0.45-dev). It also answers the points raised about the sibling
Northstar project in LWN's article
[*"Pushing Northstar"*](https://lwn.net/Articles/1102738/) and its comments.

## Summary

Every Critical and High finding is fixed on `main`, each with a pull request
that shows the bug before the fix and the same test passing after it. The
Medium and Low findings below are still open.

| Severity | Found | Fixed | Open |
|----------|------:|------:|-----:|
| Critical |     2 |     2 |    0 |
| High     |     8 |     8 |    0 |
| Medium   |    15 |     0 |   15 |
| Low      |    16 |     0 |   16 |

The worst bugs were in two places: the boundary between a page and its
frames, and the request context the network layer infers from a missing top
URL. A cross-origin frame could reach its parent's DOM. A parent form could
collect a framed login's password. Five separate paths sent another site's
`SameSite=Strict` cookies on a page-initiated request.

### Method

Seven areas were reviewed separately and every reported finding was checked
in the source before it was accepted:

- the sandbox, process model and shell/renderer IPC;
- the network layer: URLs, cookies, CORS, CSP, HSTS, caching, WebSocket, downloads;
- the JavaScript bindings and DOM;
- WebGL, WebGPU, WebAssembly, WebCrypto and WebAudio;
- CSS, layout, paint, SVG and web fonts;
- the hand-written binary parsers (ICO, WOFF2, MP4) and media decoding;
- form submission, camera permission, storage and environment hooks.

The vendored trees (lexbor, QuickJS, web-text-lib, WAMR, `subprojects/`)
were out of scope. Every fix was built with `meson compile` and tested
against a running browser: headless runs, and the GTK shell under Xvfb
driven through the built-in MCP server, with a local server logging the
requests.

## Fixed

| Severity | Issue | Fix |
|----------|-------|-----|
| Critical | A cross-origin frame reached the parent's `document` through `getRootNode()`, Document siblings and TreeWalker, and could run script as the parent | [#947](https://github.com/nordstjernen-web/nordstjernen-sol-browser/pull/947) |
| Critical | A parent `<form>` around a cross-origin frame submitted the frame's fields, a typed password included; `form="id"` crossed frames too | [#953](https://github.com/nordstjernen-web/nordstjernen-sol-browser/pull/953) |
| High | The audio helper decoded page media under a weaker sandbox: writable settings and remote-control token, Unix sockets (session D-Bus), all of `/tmp` | [#948](https://github.com/nordstjernen-web/nordstjernen-sol-browser/pull/948) |
| High | The tab icon was fetched as a browser request: Strict cookies, no third-party blocking, no Private Network Access guard, no CSP | [#949](https://github.com/nordstjernen-web/nordstjernen-sol-browser/pull/949) |
| High | A navigation from a `data:` document dropped its initiator: a forged cross-site POST looked first-party in every header | [#950](https://github.com/nordstjernen-web/nordstjernen-sol-browser/pull/950) |
| High | Enter in a login field ignored the button's `formmethod="post"`, and `method="dialog"` navigated: passwords in the URL | [#951](https://github.com/nordstjernen-web/nordstjernen-sol-browser/pull/951) |
| High | The key release after Enter erased the page-navigation marker, so a keyboard-submitted cross-site form carried Strict cookies | [#952](https://github.com/nordstjernen-web/nordstjernen-sol-browser/pull/952) |
| High | Web fonts and external SVG used `<base href>` as the requesting page: Strict cookies to any site, CSP `font-src` skipped | [#954](https://github.com/nordstjernen-web/nordstjernen-sol-browser/pull/954) |
| High | Script inside a WebGL call could switch the GL context, so `readPixels` wrote out of bounds at a page-chosen offset | [#955](https://github.com/nordstjernen-web/nordstjernen-sol-browser/pull/955) |
| High | Shrinking an animated WebGL canvas copied a stale PBO frame into the smaller surface (Windows, macOS) | [#956](https://github.com/nordstjernen-web/nordstjernen-sol-browser/pull/956) |

#956 could only be built here, not exercised, because its code path runs on
desktop GL alone. It should be run once on Windows or macOS.

## Open findings

### Sandbox and process model

- **Medium — the self-only kill rule is bypassed.** `rt_sigqueueinfo` and
  `rt_tgsigqueueinfo` are allowed unfiltered (`src/security.c`), so a
  compromised renderer can SIGKILL the shell or any process of the user on
  kernels before 6.12. Filter them on the own PID like `kill`.
- **Medium — limits and scheduling of other processes.** `prlimit64`,
  `setpriority` and the `sched_set*` calls accept any PID; one call kills the
  shell through `RLIMIT_CPU` on every kernel. Restrict them to pid 0 or the
  own PID.
- **Medium — camera and microphone nodes are always open.** Every renderer
  gets read+write on `/dev/video*` and `/dev/snd` even with both features off,
  so a compromised renderer can record without a prompt. Grant them only when
  enabled, or have the shell pass the device after a grant.
- **Medium — other applications' files are readable.** Renderers read all of
  `~/.config`, `~/.local/share`, `~/.cache`, `/run` and `/dev/shm`, which
  holds GitHub CLI, gcloud and podman credentials and other browsers'
  profiles. Narrow these to the directories the engine needs.
- **Medium — SysV IPC is allowed.** `shmget`/`shmat`/`sem*` are on the
  allow-list and Landlock does not scope them, so a renderer can attach to
  other X11 clients' MIT-SHM segments. Remove them.
- **Low — terminal access.** Renderers inherit the shell's stdin, stdout and
  stderr, and may call `setsid`, so a renderer can read keystrokes typed in
  the launching terminal. Point fd 0 at `/dev/null` and drop `setsid`.
- **Low — the no-exec-memory rule can be bypassed.** `memfd_create` plus a
  file-backed `mmap(PROT_EXEC)` gets past the filter.
- **Low — headless mode runs under the weaker shell-style profile.** Reuse
  `ns_browser_sandbox()` in `ns_headless_main()`.
- **Low — the shell opens a database renderers can write.** The shell opens
  `history.sqlite` in a directory every renderer can write. SQLite's
  defensive mode limits this.

### Network

- **Medium — `fetch('about:nordstjernen')` is readable by any page.** It
  exposes exact library versions and whether Landlock and seccomp are
  enforced. Serve `about:` pages to navigations and chrome only.
- **Medium — CSP `style-src` is never applied to inline `<style>` or
  `style=""`.** This contradicts SECURITY.md.
- **Medium — CORS preflights always carry credentials.** From a cross-site
  frame they use the frame's first-party cookie jar.
- **Low — EventSource ignores HSTS.**
- **Low — parsed URLs are never freed.** `lxb_url_destroy` is never called,
  so renderer memory grows outside the JS heap limit.
- **Low — the `-Dipc_rust` build is stale.** Its header struct no longer
  matches the C struct, and it fails closed.
- **Low — credentials survive a same-origin to cross-origin redirect** in
  `fetch()` with the default mode, and `Access-Control-Allow-Origin: *` is
  then accepted.

### JavaScript and DOM

- **Medium — sandboxed frames can navigate the top window.** A frame without
  `allow-top-navigation` can still set `top.location` from script.
- **Medium — the popup/opener bridge trusts the renderer.** It takes the
  sender's origin as the renderer reports it, so a compromised popup renderer
  can call functions in its opener's realm. The shell should stamp the origin
  itself.
- **Medium — no Permissions-Policy on camera and microphone.** Cross-origin
  frames can raise a camera or microphone prompt without `allow="camera"`.
- **Low — extension content scripts run in the page's own realm.**
  `runtime.getURL()` also reveals the on-disk path.

### GPU, WebAssembly and WebCrypto

- **Medium — the shader directive check can be bypassed.** A comment between
  `#` and `version`/`extension` skips it (`src/glsl_validate.c`), so pages can
  send GLSL ES 3.1/3.2 to the driver.
- **Medium — macOS WebGL contexts have no robust buffer access.** SECURITY.md
  says the context has robust access or none at all.
- **Low — WebGPU fingerprinting.** WebGPU reports the real GPU vendor,
  device and limits, which undoes WebGL's masking.
- **Low — WebCrypto CPU denial of service.** PBKDF2 iterations are uncapped
  and RSA keys go up to 16384 bits, all synchronous on the main thread.

### Style, layout and fonts

- **Medium — web fonts outlive their page.** They are registered for the
  whole renderer under the family name the page chose, so a page can change
  how the next site in the tab renders "Arial".
- **Medium — font bytes are written to disk without limits.**
  `FontFace(ArrayBuffer)` and `@font-face` write to
  `~/.cache/nordstjernen/webfonts` with no size, count or eviction limit, in
  private mode too.
- **Low — `:visited` is fully observable** from `getComputedStyle`.
- **Low — unbounded `border-image` tiles.** The tile count has no cap, so a
  tiny `border-image-width` freezes the renderer.

### Media and parsers

- **Low — the camera's YUYV path trusts the frame size.** It reads
  `width × height × 2` bytes without checking the driver's buffer length or
  `bytesperline` (`src/camera.c`).
- **Low — MP4 sample-table amplification.** A few kilobytes of fragmented
  MP4 can allocate about 1.2 GB of sample tables (`src/mp4.c`).
- **Low — a framed form's POST carries the parent's `Origin`** instead of the
  frame's.
- **Hardening — `ns_texture_new` does not check `stride >= width * 4`.** No
  current caller passes a short stride.

### Worth a look

These leads were not confirmed:

- pooled curl connections may skip the Private Network Access check on reuse;
- HTTPS-First falls back to `http://` silently, even after a certificate
  error;
- page-initiated top-level `data:` navigation is allowed, which can spoof the
  address bar;
- opaque `blob:null` URLs are shared across documents in a tab;
- the download staging move has a check-then-use race;
- renderers have no OS-level memory or process limits;
- the shell writes the clipboard whenever a renderer asks.

## What holds up

- **Shell/renderer boundary.** The framebuffer is a sealed memfd. Every frame
  and tile header is bounds-checked against the mapping. IPC headers, bodies
  and numbers are capped, and the session JSON parser has a depth limit.
- **seccomp allow-list.** `execve`, `ptrace`, `process_vm_*`, `bpf`,
  `io_uring`, `unshare`, `listen` and `accept` are absent, and `clone` is
  filtered.
- **Remote control.** It is off by default. It listens on loopback only and
  needs a 256-bit token compared in constant time on every request.
- **TLS and redirects.** libcurl verifies peers and hosts. Protocols are
  allow-listed. Each redirect hop is re-checked, and POST redirects never move
  the body into the URL.
- **Cookies.** Prefixes, `Secure`, `HttpOnly`, `SameSite=None` requiring
  `Secure`, and public-suffix checks are enforced.
- **Headers and decompression.** CRLF header injection is blocked, and
  decompression is capped per stage and in total.
- **Canvas tainting.** It is enforced on every read-back path. WebGL and
  WebGPU uploads refuse tainted sources, and an SVG image can only embed
  `data:` images.
- **DOM lifetime.** Removed nodes are freed only after GC, and only when no
  wrapper remains. Every `JS_GetOpaque` call passes a class id.
- **Typed arrays.** Buffer pointers are taken after every script-running
  coercion.
- **Parsers.** ICO, WOFF2 and MP4 check every offset in 64-bit or
  subtraction form. Image dimensions, pixel counts and frame counts are
  capped.
- **Storage.** All SQL is constant or uses bound parameters. IndexedDB file
  names are hashes in 0700 directories. The bytecode cache is in memory only
  and keyed on the full source.
- **WebAssembly.** WAMR is interpreter-only with bounds checks, and
  `Memory.buffer` is detached on every grow.
- **Supply chain.** Every subproject wrap is pinned by commit or SHA-256, and
  the AppImage tools are checksum-verified.

## The LWN article and its comments

The article covers the attempt to package Northstar, which shares this
engine, in Debian and the AUR. A frontier-model audit run by Ted Ts'o
reported eight critical and eight high issues there. Among them were
undocumented environment hooks, including one that silently granted webcam
access, and differences between the documented security model and the code.
The points that apply to Nordstjernen, and what this review did with them:

1. **A password in the URL bar after a failed login (linuxrocks123).** This
   reproduces here. Enter in a login field ignored the button's
   `formmethod="post"` and loaded `/login?user=…&pw=…`, and a dialog form did
   the same. Both are fixed (#951). One more safeguard is still worth adding:
   refuse, or ask before, a GET submission that carries a non-empty password
   field.
2. **Undocumented hooks and silent webcam access.** Nordstjernen has no
   environment variable or setting that grants a capability without a
   prompt. Every security-relevant `NS_*` switch is in SECURITY.md. Two gaps
   remain:
   - `NS_NET_TRACE`, which prints `Cookie` and `Authorization` headers to
     stderr, and `NS_NET_LOG`, `NS_FORCE_HTTP3` and `NS_PAGE_FOR_COMPUTER`
     are undocumented.
   - Renderers keep the camera and microphone device nodes open regardless of
     the setting, so the prompt is enforced only inside the untrusted
     process.
3. **"Discrepancies between the claimed security model and the code."** This
   applies here too. Besides the audio helper (fixed in #948), SECURITY.md
   overstates four things, and the code or the text should change:
   - inline `style-src` enforcement;
   - the self-only kill rule;
   - "anonymous executable mmap is refused";
   - robust buffer access on macOS.

   Checking SECURITY.md against the code on each release would catch drift
   like this.
4. **Churn and auditability (Gard Spreemann, calvin).** A codebase that
   changes this fast is hard to audit. Some steps would make audits feasible:
   - Name owners for the files that hold security boundaries: `security.c`,
     `net.c`, `renderer_serve.c`, the frame and form code, and `webgl.c`.
     Require a human review of every change to them.
   - Publish an advisory per release from the `Security-Fix:` trailers. This
     review adds ten; the tree now has 21.
5. **Long-term support (Marc Haber).** SECURITY.md supports only the latest
   release. A distribution needs a maintained branch that receives backported
   security fixes. The `Security-Fix:` and `Ported-From:` trailers already
   make that tracking possible.
6. **Testing.** Several commenters treat the absence of a test suite as a
   warning sign. The project's policy rules out test suites and fuzzing, so
   this is the maintainer's call. Ten fixes in this review came with a small
   reproducer page that shows the bug before the fix and its absence after.
   A sanitizer build (ASan and UBSan) in CI that loads such pages would catch
   regressions of exactly these bugs without a test framework.
7. **Scope (claudex, roc).** A smaller default surface is a security feature.
   WebGPU is labelled experimental yet is on by default. Two of the High
   bugs were in WebGL, and the GPU drivers behind both APIs force the
   renderer to allow executable memory. Turning WebGPU off by default, and
   asking before a site gets WebGL, would shrink what an ordinary page can
   reach.
8. **Supply chain.** Northstar's packaging script fetched unverified
   binaries. Here every download is pinned and checksum-verified.
9. **"Shares the engine" and "written from scratch" (phm).** README and the
   architecture document say both, while the parser, JavaScript engine, image
   decoders and WebAssembly runtime are vendored libraries. The architecture
   document's wording makes the split clear: "original engine code over
   vendored libraries". README could use the same wording.
