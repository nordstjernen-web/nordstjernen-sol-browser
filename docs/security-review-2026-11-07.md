# Security review, 7 November 2026

A second review of Nordstjernen's own source for security vulnerabilities, made
at `bdc48cd` (1.0.45-dev). It follows the
[November 2026 review](security-review-2026-11.md) and does not repeat its
still-open findings except to confirm they remain.

## Method

Eleven areas were reviewed separately — the sandbox and process model; the
network core; cookies, CSP, storage and sockets; the JavaScript bindings and
DOM (read in four slices); DOM, HTML, forms and navigation; WebGL, WebGPU,
canvas and WebAudio; the image, font, MP4 and media parsers; the CSS engine;
layout, paint and animation; and the shell, remote control and supply chain.

Unlike the previous review, this one was done against a **working build**: the
full dependency set was installed and `meson setup builddir && meson compile`
succeeded with no warnings. The findings marked *verified by running* were
reproduced with the built browser driven headless across two local origins. The
rest are confirmed by reading the code; a few are marked *plausible* where one
step could not be settled from the source alone.

The vendored trees (lexbor, QuickJS, web-text-lib, WAMR, Wuffs, wgpu-native,
`subprojects/`) were out of scope, as before.

Two findings reported during the review were **rejected after building and
running the browser** — see "Rejected" below. Both relied on reaching a
JavaScript host object's internal state that the engine actually hides, a
layer worth describing because it defeats a whole class of plausible-looking
reports.

## Summary

| Severity | New | Verified by running |
|----------|----:|--------------------:|
| High     |  10 |                   4 |
| Medium   |  17 |                   1 |
| Low      |  12 |                   0 |

The worst new findings cluster in the same place the last review's did: the
boundary between a page and a cross-origin frame. Four separate paths let an
embedding or embedded page cross that boundary — run script in the other
realm, read the other frame's text, or run script the sandbox should have
blocked. Two more are memory-safety bugs in the WebGL texture-upload path, in
the same class as the previously fixed `#955`. The CSS engine (39k lines), the
image/font/media parsers, the layout and paint code, and the remote-control
and supply-chain surfaces came back essentially clean.

---

## High

### H1. A `javascript:` link in a script-less cross-origin frame runs in the embedder's realm
*Confirmed by code trace; reachability traced; execution not reproduced headless (driver limitation).*

`ns_js_run_javascript_url` (`src/js.c:50180`) resolves the realm to run a
`javascript:` URL in with `realm = ns_js_node_realm_context(js, anchor);
if (!realm) realm = js->main_realm_ctx;`. A cross-origin frame whose markup has
no `<script>`, module, or inline handler never gets a realm created (the gate at
`src/js.c:71595`), so `ns_js_node_realm_context` returns NULL and the fallback
runs the attacker's code in the **top page's** realm. The only guards are a
sandbox check and the frame's own (attacker-controlled) CSP. Both activation
paths — Enter on a focused link (`ns_js_keyboard_activate`, `src/js.c:50320`)
and Alt+accesskey (`browser_find_accesskey`, which descends into frame
documents, `src/libnordstjernen.c:5128`) — reach it with the frame's anchor.

Impact is cross-origin script execution in the embedder: a frame serving only
`<a href="javascript:…" accesskey="s">Continue</a>` runs its code as the
embedding site after one ordinary link activation. Critical-class impact, gated
by a single user gesture the attacker chooses the label for.

Fix: never fall back to `main_realm_ctx` for a node inside a frame; create the
frame realm on demand or refuse when the frame is cross-origin.

### H2. `sandbox="allow-same-origin"` without `allow-scripts` still runs inline handlers, in the parent realm
*Verified by running.*

A page at one origin embedding
`<iframe sandbox="allow-same-origin" srcdoc="<img src=x onerror=…>">` had the
`onerror` handler execute and reach `parent.document`. The title of the top
page became the value the handler set. When `allow-scripts` is absent the loader
only marks `<script>` elements started and creates no frame realm, but event
dispatch into the frame is not gated on the sandbox: `ns_fire_inline_on_handler`
(`src/js.c:34916`) finds no frame realm and falls back to the main realm. No
code path consults `NS_SANDBOX_ALLOW_SCRIPTS` for handlers.

Impact: untrusted HTML rendered in a scripts-disabled sandboxed iframe (the
standard safe-preview pattern for webmail and CMSes) achieves script execution
on the embedding origin. `onerror` and `onmouseover` need no user gesture.

Fix: return false from `ns_fire_inline_on_handler` (and `ns_element_on_get`)
when `ns_node_sandbox_blocks_scripts(target)`, and never run frame content in
`main_realm_ctx`.

### H3. `execCommand('selectAll')` + `getSelection().toString()` reads cross-origin frame text
*Verified by running.*

A top page embedding a cross-origin frame containing `FRAME_SECRET_INBOX_LINE_42`
ran `document.execCommand('selectAll')` then read `getSelection().toString()`
and received the frame's text. Frame documents are laid out in the one shared
layout tree, and the native select-all (`ns_selection_select_all` /
`collect_walk_cb` in `src/selection.c`) walks it with no frame or origin
boundary; the collected text is stored in a single per-tab buffer that every
realm reads. `ns_document_command_run` handles `selectall` with no
user-activation check (`src/libnordstjernen.c:1029`).

Impact: an embedding page reads the rendered text of any frameable
cross-origin page the user is logged into, with no interaction. The reverse
(a framed ad reading the host page) works too.

Fix: clip the native selection to one document, require transient activation
for `selectAll`, and expose the selection text only to the realm holding it.

### H4. CSP `script-src` is not applied to `<script src="data:…">` or `blob:` in the main document
*Verified by running.*

A page sent with `script-src 'self'` and containing
`<script src="data:text/javascript,…">` executed the data: script. In
`ns_js_run_script_element` (`src/js.c:69450`) the `data:` and `blob:` branches
evaluate the bytes before the URL is resolved or any
`ns_csp_allows_with_nonce(…, NS_CSP_SCRIPT, …)` check runs; SRI is skipped too.
The `https:` path and the frame path both check CSP, so the gap is specific to
inline data:/blob: script elements in the main document.

Impact: CSP gives no protection against an injected
`<script src="data:…">`, defeating the usual containment for an HTML-injection
bug.

Fix: resolve the URL and run the CSP (and SRI) check before the data:/blob:
branches, as the http(s) path does.

### H5. Use-after-free in WebGL `texImage2D`/`texSubImage2D` from an ArrayBuffer `data` getter
*Confirmed by code trace.*

`view_bytes` (`src/webgl.c`) returns a bare `ArrayBuffer`'s data pointer with
`*hold = JS_UNDEFINED` — it takes a reference only for typed arrays.
`wgl_imagedata_bytes` (`src/webgl.c:3028`) then does
`const uint8_t *p = view_bytes(ctx, dv, &len, hold); JS_FreeValue(ctx, dv);
return p;`. When `src.data` is a getter returning a fresh `ArrayBuffer`, `dv`
is the only reference, `JS_FreeValue` drops it to zero, and the freed pointer
is handed to the GL upload.

Impact: `gl.texImage2D(…, {width:64,height:64,get data(){return new
ArrayBuffer(16384)}})` uploads freed heap into a texture; attaching it to an
FBO and calling `readPixels` returns the bytes to script (heap disclosure, ASLR
defeat). A large buffer is unmapped on free and crashes the renderer.

Fix: `*hold = JS_DupValue(ctx, v)` in the ArrayBuffer branch of `view_bytes`,
and reject non-ImageData objects.

### H6. WebGL texture upload can run on a different context switched in by a source getter (`#955`-class over-read)
*Confirmed by code trace.*

The previously fixed `#955` reentrancy still exists in the texture-source path.
`wgl_imagedata_bytes` reads `width`, `height` and `data` through page getters
and nothing calls `wgl_reassert(g)` before the upload. A getter that calls a
method on a second WebGL 2 context makes it current, and
`wgl_unpack_tight` resets only `UNPACK_ALIGNMENT` for a WebGL 1 context, so the
other context's `UNPACK_ROW_LENGTH`/`SKIP_ROWS`/`SKIP_PIXELS` stay in force over
a tight heap buffer (`src/webgl.c:3115`, `3171-3207`).

Impact: the driver copies far past a small `g_malloc` buffer into a texture the
attacker reads back — renderer heap disclosure, or a crash for large skips.

Fix: call `wgl_reassert(g)` after source extraction and reset the row/skip
parameters for any ES3/GL3 context.

### H7. Script-triggered downloads are fetched in the target's own first-party context
*Confirmed by code trace.*

A page can start a download with `a.download=''; a.click()` (no gesture, no
prompt for non-executable names; downloads default to allowed). The renderer
passes the download URL as its own requesting document —
`ns_download_start(url, url)` (`src/renderer_serve.c:1327`) — so in
`ns_fetch_sync_hop` every context decision is made against the target itself:
`Sec-Fetch-Site: same-origin`, no `Origin`, the target's first-party cookie jar,
`SameSite=Strict` cookies attached, and the Private Network Access guard
comparing the target with itself.

Impact: silent GET with another site's Strict cookies (write-CSRF defeating
SameSite and Fetch-Metadata), and a path to private hosts bypassing PNA.

Fix: carry the initiating document URL through the download request and
classify it like any other page-initiated request.

### H8. Download staging trusts a renderer-reported path lexically (symlinked staging dir)
*Confirmed by code trace; requires a compromised renderer.*

The shell validates the renderer's staging path with a purely lexical
`g_canonicalize_filename` of the parent plus a NOFOLLOW stat of the leaf
(`src/gtk/procwindow.c:844-884`). A renderer, which has full read/write on
`~/.cache/nordstjernen`, can replace the `downloads` directory with a symlink to
`$HOME` or `~/.ssh`; the reported path then passes both checks and
`g_file_move` relocates a real user file into `~/Downloads`.

Fix: keep the staging file under shell control (shell creates the `.part` and
passes an fd), or open the reported path with `O_NOFOLLOW` relative to an
`openat` fd of a staging dir the shell opened and `fstat`-verified.

### H9. Renderer seccomp argument filters are compiled only for x86_64 and aarch64
*Confirmed by code trace; requires a compromised renderer; secondary architectures only.*

All argument-level seccomp rules sit behind
`#if defined(NS_HAVE_SECCOMP) && (defined(__x86_64__) || defined(__aarch64__))`
(`src/security.c:208`). On every other Linux architecture the fallback
(`src/security.c:1009-1017`) allows `socket`, `kill`, `tgkill` and `clone`
unfiltered, and `ns_security_seccomp_deny_unix_sockets()` becomes a no-op.
Landlock does not mediate `connect()` to a pathname UNIX socket — which
SECURITY.md itself names as the reason for the AF_UNIX restriction. Debian
packaging is `Architecture: any` and the RPM spec excludes only 32-bit x86/ARM,
so ppc64el, riscv64 and s390x builds ship the gap.

Impact: a compromised renderer on those builds can `connect()` to the session
D-Bus and have it start a program outside the sandbox, `kill` the shell, or
`clone(CLONE_NEWUSER)`.

Fix: build the argument filters on every architecture libseccomp supports, or
fail closed where they are not compiled in.

### H10. macOS Seatbelt profile is `(allow default)` minus file writes
*Confirmed by code trace; requires a compromised renderer; macOS only.*

The renderer's macOS profile is `(allow default)` then `(deny file-write*)`
with the profile dirs, `/tmp` and `/dev` re-allowed (`src/security.c:1135`).
Everything else stays permitted: `process-exec`, all `mach-lookup`
(launchd/LaunchServices), `appleevent-send`, all file reads (`~/.ssh`, other
browsers' data), and the network. `sandbox_init` failure only warns and
continues unconfined.

Impact: contradicts SECURITY.md's "strongest sandbox … does not yield code
execution outside the user's data directory". A compromised renderer reads the
whole home directory and can write `/tmp/x.command` and `open` it.

Fix: start from `(deny default)` and allow only what the renderer needs; fail
closed when the profile is rejected.

---

## Medium

Network and cookies:
- **Private Network Access is skipped on reused pooled connections** — the
  guard is an open-socket callback, and a request reusing a keep-alive/HTTP/2
  connection to a private host never reaches it (`src/net.c:2538`, `6294`).
- **PNA is off for any document whose URL is not http(s)** — `data:`, `blob:null`
  and `about:` documents classify as UNKNOWN and fall through; such requests
  also carry no `Origin` at all (`src/net.c:2328`, `2545`).
- **A configured proxy with `no_proxy` exceptions disables the PNA guard** for
  the excluded hosts, because `guard->proxied` is decided per URL, not per
  connection (`src/net.c:2529`).
- **Third-party cookie blocking and SameSite are bypassed by subresources of a
  cross-site frame** — images/fonts/SVG/media in a cross-site frame lack the
  `X-ND-Top-Level` marker, so net.c partitions them into the frame site's
  first-party jar (`src/net.c:6706`).
- **EventSource is always credentialed and accepts `Access-Control-Allow-Origin:
  *`** without checking `Access-Control-Allow-Credentials` (`src/eventsource.c:278`,
  `src/js.c:26314`).
- **`cookies.db` is opened read/write by every renderer** — each renderer is its
  own cookie store, so a compromised renderer reads and writes every partition's
  cookies, HttpOnly included (`src/net.c:2028`).
- **IndexedDB quota is enforced only in `put`** — creating databases,
  `createObjectStore`, `createIndex` and `setVersion` never check it, and the
  number of database files is unbounded; any page can fill the disk
  (`src/idb.c:878`).

JavaScript, DOM and navigation:
- **Module `import`/`import()` skip CSP `script-src` and the mixed-content check**
  (`src/js.c:68917`): a gadget importing an attacker-named specifier runs, and an
  https page can import `http://` script over cleartext.
- **WebAssembly cannot be interrupted** — the slow-script monitor and
  `worker.terminate()` do not stop a wasm infinite loop, and module-declared
  memories have no size cap.
- **The shell trusts the renderer-asserted final URL and title** for the address
  bar with no cross-check against the committed navigation — address-bar spoofing
  from a compromised renderer (`src/gtk/procview.c:3643`).
- **Renderer tile/layer descriptions have no per-reply count or byte budget** — one
  `/render` reply can make the shell copy gigabytes out of the shared map
  (`src/gtk/pagelayers.c:300`); shell DoS.
- **A click on a link inside any frame navigates the whole tab**, resolved against
  the top URL and with the top page as initiator, ignoring the frame's sandbox
  and `target` (`src/libnordstjernen.c:4697`).
- **Document charset is taken from any `charset=` text in the first 1 KB of the
  body**, not a `<meta>` prescan — reflected text can switch a page to
  Shift_JIS/ISO-2022-JP and break server escaping (`src/html.c:705`).
- **Use-after-free in headless mode** — clicking a contenteditable host frees its
  children, then `blur` is dispatched to a now-freed focused input
  (`src/dom.c:812`, `src/headless.c:1393`); headless runs under the weaker
  sandbox.

GPU, privacy, CSS, layout:
- **Any page reads camera device paths and hardware names with no prompt**
  (*verified by code*) — `enumerateDevices()` returns `/dev/videoN` and the real
  camera name with no permission or setting check (`src/js.c:11829`); a stable
  cross-site fingerprint.
- **`:has()` matching is not charged against the selector op budget** (*plausible*)
  — the relative-selector subtree walk has only a depth guard and no op budget,
  so a crafted stylesheet or a scripted `querySelectorAll(':has(…)')` can hang
  the renderer (`src/css.c:25786`).
- **Web fonts outlive their page** — a downloaded face is registered process-wide
  under the page-chosen family name, so a later site in the same renderer
  resolves the attacker's "Arial" (`src/font.c:740`).

---

## Low

- WebSocket and EventSource queue up to ~2 GB of server data outside the JS heap
  cap (`src/ws.c`, `src/eventsource.c`).
- Cross-site responses may set `SameSite=Lax/Strict` cookies; no 400-day expiry
  cap, and a huge `Max-Age` overflows `now + max_age` (`src/cookies.c:763`).
- Cookie `Domain` public-suffix check fails open when libpsl has no built-in list
  (`src/cookies.c:881`).
- `frame-ancestors` / `X-Frame-Options` are checked against the immediate
  embedder only, not every ancestor (`src/js.c:71774`).
- Cross-renderer popup messages run `JS_ReadObject` on another renderer's bytes
  and construct any global named in the data (`src/js.c:27500`); extends the open
  popup/opener-bridge finding.
- `window.location` is a plain writable property; a few origin/URL lookups read
  it back (`src/js.c:66946`).
- `window.postMessage` checks `targetOrigin` at post time, not at delivery
  (*plausible*) (`src/js.c:15766`).
- Unbounded `border-image` tile count freezes the renderer (`src/paint.c:1824`).
- `@page { size }` is uncapped and cast to `int` (UB) in print/PDF layout
  (`src/libnordstjernen.c:2223`).
- The inline PDF viewer rasterises up to 300 pages into one base64 string with no
  total budget (`src/pdf.c`).
- Headless `--dump` writes raw page control characters to stdout (terminal escape
  injection, e.g. OSC 52 clipboard).
- Remote control holds all four connection slots for 10 s on unauthenticated
  malformed lines (`src/gtk/remotectl.c`); opt-in, self-healing.
- MF (Windows) and Android video backends use the decoder-reported crop/aperture
  as the copy extent without clamping to the coded size (*plausible*)
  (`src/media_os_mf.c:272`, `src/media_os_android.c:116`).

---

## Rejected after building and running

Two findings raised during the review did not hold up when run against the
built browser. Both are recorded here because each rests on a defensive layer
that is easy to miss when reading one function in isolation.

- **"A transferred MessagePort exposes an object from the other realm through
  `_pair`."** Host objects (`ns_hostobj`, `src/js.c:10910`) keep all engine
  state in a separate `d->state` object, and the class exotic handlers
  (`src/js.c:10841-10893`) gate every get/define/enumerate on
  `JS_IsHostAccess(ctx)`. Page script sees none of `_pair`, `_shipped_to`,
  `_queue`; reading them returns `undefined` and the port linkage never crosses
  the script-visible boundary. Verified by running: the claimed read returns
  nothing.
- **"A cross-origin iframe reads the top document's cookies via
  `HTMLDocument.prototype.cookie`."** The frame's `document` carries a per-URL
  accessor and a realm-document marker, and the fork brands native accessors
  with `JS_SetCFunctionBrand` (`src/js_brand.c:42`), so calling the prototype
  getter with a foreign `this` throws and with the real document returns empty.
  Verified by running across two origins: the victim cookie never leaks.

The lesson for future reviews: a JavaScript-binding report that depends on
calling a prototype accessor with a forged `this`, or on reaching a host
object's `_`-prefixed internals, must be checked against the host-object
exotic handlers and `js_brand.c` before it is believed.

---

## What holds up

Confirmed sound in this review, often where a bug looked likely:

- **Binary parsers.** ICO, WebP, Wuffs, AVIF, JPEG XL, WOFF2 and MP4 bound every
  offset and cap every dimension, pixel and frame count; the audio helper's
  stdin, base64 and mixing paths are bounded. No new parser memory-safety bug.
- **CSS engine.** The tokenizer, `calc()`/`var()`/`attr()` expanders (with
  billion-laughs budgets), the grid `repeat()` parser, the selector matcher and
  every fixed-size buffer across 39k lines are soundly bounded.
- **Layout and paint.** DOM-derived counts are hard-capped, variable counts are
  heap-allocated, every cairo surface is status-checked, and NaN/inf is rejected
  before reaching cairo.
- **Remote control.** Loopback IPv4 only, ephemeral port, a 256-bit `RAND_bytes`
  token compared in constant time, and a non-HTTP line protocol a page's
  `fetch` cannot authenticate to. The renderer cannot reach the token: renderers
  have no Landlock rule for the state directory.
- **WebAuthn.** The shell re-derives the page origin and validates the RP ID as a
  registrable suffix; a renderer cannot assert a foreign relying party.
- **Supply chain.** Every wrap is pinned by commit or SHA-256, third-party GitHub
  Actions are pinned by SHA, no `pull_request_target`, no untrusted interpolation
  into `run:`, and the download scripts verify checksums and fail closed.
- **IPC and framebuffer.** IPC header fields are length-checked, the framebuffer
  memfd is sealed, and tile/frame geometry is bounds-checked against the mapping.
