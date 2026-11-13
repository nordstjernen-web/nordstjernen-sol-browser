# Security review report

Review of Nordstjernen at commit `21339ac` (1.0.47-dev, 2026-11-13), looking
for security vulnerabilities in the first-party source. It follows
`docs/code-review-report.md` (commit `6ac1c8c`) and concentrates on what that
review left open: the JavaScript binding, layout and paint, the web-facing
policy logic (same-origin, CORS, CSP, cookies, iframe sandbox, `file:`), and
WebAssembly. The review changed no source file.

## 1. Summary

- **No memory-corruption bug was found.** An AddressSanitizer +
  UndefinedBehaviorSanitizer build of the renderer ran hand-written lifetime
  stress pages and two fuzzers without a single ASan report.
- **Five web-policy findings were confirmed by running them** against the
  built renderer. The most serious lets a page read fragments of
  cross-origin, cookie-authenticated resources through script error messages
  (F1). Three are Content-Security-Policy gaps (F2–F4).
- **Undefined behaviour exists but did not reach memory**: out-of-range
  float-to-integer conversions in layout and paint, and signed overflow in
  the text-layout fork (F7).
- **The origin model held up.** About 200 probes of cross-origin frame
  access, CORS, opaque responses, canvas tainting, cookies, sandboxed frames,
  privileged `about:` pages, `file:` access and request-header smuggling
  found nothing beyond the items below.

| # | Finding | Attacker | Severity | Status |
|---|---------|----------|----------|--------|
| F1 | Errors from cross-origin scripts are reported unmuted | any web page | medium | fixed after this review |
| F2 | CSP `style-src` is not applied to inline `<style>` or `style` attributes | injected markup on a CSP-protected site | medium | fixed after this review |
| F3 | `import()` of `data:` and `blob:` modules skips CSP `script-src` | script on a CSP-protected site | low | confirmed |
| F4 | CSP `'self'` matches `blob:` URLs (workers under `worker-src 'self'`) | script on a CSP-protected site | low | confirmed |
| F5 | A page may navigate the top level to a `data:` URL | any web page | low | confirmed |
| F6 | `structuredClone` / `postMessage` transfer detaches a `WebAssembly.Memory` buffer | any web page | low (no memory impact seen) | confirmed |
| F7 | Undefined float-to-int conversions and signed overflow on hostile CSS | any web page | low (hardening) | confirmed by UBSan |
| F8 | Internal state kept in `data-nd-*` DOM attributes behind a name filter | — | design note | filter held in 30 probes |

Two items of the earlier report are still present in this commit: the
Landlock read set still includes `/proc`, `/sys` and `/dev/shm` (S4,
`src/security.c:599-601`), and private mode still falls back to a
predictable directory in `/tmp` (S11, `src/net.c:486-498`).

## 2. Method and coverage

**Build.** A copy of the tree was built outside the repository with
`clang -fsanitize=address,undefined`, `-Dgtk=disabled -Dwebgpu=disabled
-Dpdfjs=disabled -Dwasm_simd=disabled`. The copy needed local build-file
changes that do not belong in the tree: the libcurl floor lowered to the 8.5
that Ubuntu 24.04 ships (so WebSocket was not exercised), `_FORTIFY_SOURCE`
off and the `exe-local.map` version script dropped (both conflict with
ASan's allocator interception). The GTK shell was not built or run.

**Dynamic tests**, all through `nordstjernen-renderer --headless-host`:

| Test | What it covered | Result |
|------|-----------------|--------|
| 8 lifetime stress pages | node removal inside event, focus, toggle, mutation-observer, custom-element and slot callbacks; NodeIterator/TreeWalker/Range under mutation; frames removed while loading or unloading; `document.open`; CSSOM rules used after deletion; animations on removed nodes; canvas resized inside `valueOf`/getters; resizable and transferred ArrayBuffers through `crypto`, `TextDecoder`, streams, Blob, WebAudio; worker termination and port transfer races | no ASan report |
| WebAssembly pages | `Memory.grow` with live views, buffer transfer and resize, module validation | no ASan report; F6 |
| Random HTML/CSS/SVG/MathML generator with DOM mutation scripts, layout and PNG paint | about 25 minutes on 4 cores | no ASan report; F7 |
| JavaScript API-surface fuzzer (random method calls on every reachable interface object, with hostile arguments) | about 20 minutes on 3 cores | no ASan report; hangs were script-level infinite loops |
| Two-origin HTTP server (`127.0.0.1:8001` / `:8002`) with policy probe pages | section 4 | F1–F5 |

WebAssembly code could not be *executed* under ASan (WAMR's native stack
guard trips on ASan's larger frames), so instances, tables, globals and the
import/export call bridge in `src/wasm.c` were not exercised; only its
memory lifetime code was read.

**Read by hand:** `src/cache.c`, `src/content_decode.c`, `src/image_webp.c`,
`src/csp.c`, the WebVTT parser in `src/video.c`, the buffer-access helpers
and `putImageData`/`ImageData` paths in `src/js.c`, `src/js_canvas.c`,
`src/js_canvas_api.c`, the memory lifetime code of `src/wasm.c`, `file:`
access control and CORS helpers in `src/net.c`, the WebDriver request gate,
and the shell's audio-helper command filter.

**Not covered:** the GTK shell at runtime (popups, permissions, downloads,
clipboard, URL bar), WebGL and WebGPU (no GPU in the container), WebSocket,
HTTPS-only behaviour (HSTS, mixed content, `Secure` cookies over real TLS),
media decoding paths, macOS and Windows sandboxes, and the forked third-party
trees beyond what the fuzzers reached.

## 3. Findings

### F1. Errors from cross-origin scripts are reported unmuted (medium)

A classic `<script src>` may load any cross-origin URL without CORS, and the
request carries the user's cookies. HTML requires errors from such a script
to reach `window.onerror` as the bare string `"Script error."`.
`ns_js_report_error_event` (`src/js.c:68964-68993`) has no origin check: it
always delivers the real message, file name and the `error` object, whose
`stack` holds the position. `importScripts` in a worker rethrows the full
message the same way.

Confirmed: `http://127.0.0.1:8001` loaded
`<script src="http://127.0.0.1:8002/f/secret.csv">`, where the file holds
`topsecretpassword,alice,1234`, and its `onerror` handler received

```
msg=ReferenceError: 'topsecretpassword' is not defined
url=http://127.0.0.1:8002/f/secret.csv
stack=    at <eval> (http://127.0.0.1:8002/f/secret.csv:1:1)
```

Impact: any response that parses as far as an identifier leaks that
identifier (CSV exports, plain-text tokens, some JSON-with-prefix formats),
and the syntax-error position leaks structure of the rest.

The same handler on a `file:` page reads the first token of sibling files,
which `fetch` and XHR correctly refuse it. Through a symbolic link in the
page's own directory it also reads outside that directory: a link to
`/etc/hostname` returned the host name, because `ns_file_access_allowed`
(`src/net.c:4808-4826`) compares `g_canonicalize_filename` output, which
does not resolve links. That needs a link next to the page, such as one
unpacked from an archive.

Fix: record on each compiled script whether it was fetched cross-origin
without CORS (the element's `crossorigin` state is already known at
`src/js.c:51547`). For such a script report `"Script error."`, an empty file
name, zero line and column and a `null` error, and give `importScripts` a
generic `NetworkError`-style message.

### F2. CSP `style-src` is not applied to inline styles (medium)

`NS_CSP_STYLE` is checked only where a style sheet is fetched
(`src/engine.c:544`, `:758`, `:1620`). No call site checks a `<style>`
element's text or a `style` attribute, and `src/csp.c` has an inline check
for scripts only (`ns_csp_inline_script_allowed`).

Confirmed with `style-src 'nonce-abc'`: a `<style>` block without the nonce
and a `style="…"` attribute both applied.

Impact: on a site that relies on CSP to contain HTML injection, injected
`<style>` can still restyle the page and read attribute values (CSRF tokens,
form values) with attribute selectors. The exfiltration request itself is
still subject to `img-src`/`font-src`, which limits this on strict policies.

Fix: add `ns_csp_inline_style_allowed(csp, body, len, nonce, is_attribute)`
mirroring the script version (`'unsafe-inline'`, nonce and hash for
`style-src-elem`; `'unsafe-inline'` and `'unsafe-hashes'` for
`style-src-attr`) and call it where `<style>` text and `style` attributes
enter the cascade. CSSOM writes from script stay allowed.

### F3. `import()` of `data:` and `blob:` modules skips `script-src` (low)

In the module loader the `data:` branch and the `blob:` branch (the latter
at `src/js.c:70618-70646`) compile and return before the
`ns_csp_allows(csp, NS_CSP_SCRIPT, …)` check at `src/js.c:70662`, which only
http(s) specifiers reach.

Confirmed with `script-src 'nonce-abc'` (no `'strict-dynamic'`):
`import('data:text/javascript,hit("import-data")')` ran, while `eval`,
`Function`, string timers, inline handlers, `javascript:` URLs, `blob:` and
`data:` `<script src>` were all blocked.

Impact: this is `eval` by another name on pages that deliberately withhold
`'unsafe-eval'`; it needs a script that passes attacker-influenced text to
`import()`.

Fix: run the same `ns_csp_allows` check for `data:` and `blob:` specifiers
before compiling.

### F4. CSP `'self'` matches `blob:` URLs (low)

`source_matches` evaluates `'self'` with `ns_url_same_origin`
(`src/csp.c:250`), and the origin of `blob:http://host/uuid` is `http://host`.
CSP3 matches `'self'` against the URL's scheme, host and port, so `blob:`
and `filesystem:` never match and need an explicit `blob:` source.

Confirmed: under `worker-src 'self'` a `new Worker(blobURL)` started.

Fix: return FALSE from the `'self'` branch when the resource URL is not a
network scheme.

### F5. Top-level navigation to `data:` from page script (low)

`location.href = 'data:text/html,<script>…</script>…'` from an http page
loaded and ran the document at the top level. Redirects to `data:`, `file:`,
`about:` and `view-source:`, and script navigation to `file:`, privileged
`about:` pages and `view-source:` are all refused; `data:` is the one
exception.

Impact: phishing pages with no origin in the URL bar, and content that
filters keyed on the page URL cannot attribute. Other browsers have blocked
this since 2017.

Fix: refuse renderer-initiated top-level navigation to `data:` in the same
place that already refuses `file:` (`blocked navigation: scheme not
allowed`); keep it for address-bar entry and for frames.

### F6. Transfer detaches a `WebAssembly.Memory` buffer (low)

`ArrayBuffer.prototype.transfer()` and `resize()` correctly refuse a buffer
that backs a `WebAssembly.Memory` ("cannot transfer the buffer of a
WebAssembly.Memory"). The structured-clone transfer list does not:
`structuredClone(m.buffer, {transfer: [m.buffer]})` and
`port.postMessage(m.buffer, [m.buffer])` both succeed. The receiver gets a
copy and the original is detached with `JS_DetachArrayBuffer`, behind the
back of `ns_wasm_memory`, whose `buffer`, `base` and `size` fields
(`src/wasm.c:664-674`, `:705-727`) still describe the old state — so
`m.buffer` keeps returning the detached object until the next `grow`.

No memory error followed in testing: the linear memory is owned by WAMR and
the ArrayBuffer was created without a free function, and growing afterwards
re-creates the buffer correctly. It is reported because it is an invariant
the module otherwise defends, and a later change that trusts `m->base` after
a detach would turn it into a use-after-free.

Fix: make the structured-clone transfer path consult the same
`ns_wasm_buffers` registry (`src/wasm.c:655-661`) and throw `DataCloneError`.

### F7. Undefined conversions on hostile CSS (low, hardening)

UBSan reports on fuzzed pages, each reached from plain CSS lengths:

| Site | Problem |
|------|---------|
| `src/layout.c:7469-7470` | `(int)(top * NS_PANGO_SCALE)` and siblings with `nan`, `±1e13` |
| `src/paint.c:217` | `(int)(r->box_w * NS_PANGO_SCALE)` with `1e42` |
| `src/libnordstjernen.c:218-219` | `(gint32)(b->x * 4)` with `nan`, `inf` |
| `src/libnordstjernen.c:2767` | `(int)bottom + ypad` with `2.8e9` |
| `src/js.c:42949` | `(int)(h + 0.5)` with `nan` |
| `src/web-text-lib/pango-layout.c` (`:1807`, `:1823`, `:3058`, `:4955`, `:5906` …), `pango-renderer.c`, `pango-utils.c` | signed overflow on `INT_MIN` Pango units that the casts above produce |

On x86-64 the cast yields `INT_MIN` and nothing indexed memory with it. It is
still undefined behaviour, the result differs on AArch64 (saturation), and
`page_size_h` derived this way feeds framebuffer geometry. Clamp box
geometry to a finite range once, where layout produces it (a
`ns_px_to_pango()` helper that saturates and maps NaN to 0), rather than at
each cast. `-fwrapv`, already recommended as B1 in the earlier report,
removes the signed-overflow half.

UBSan also reports `js_realloc_rt` called through a mismatched function
pointer type (`src/quickjs-bellard/cutils.c:119`, `:209`). It is harmless
today and would abort under `-fsanitize=cfi-icall`.

### F8. Internal state in `data-nd-*` attributes (design note)

The engine stores security-relevant per-element state as ordinary DOM
attributes: `data-nd-frame-url` (the URL a frame's document is treated as
having), `data-nd-frame-srcdoc`, `data-nd-csp-sandbox`, `data-nd-frame-csp`,
`data-nd-shadow-root`, and others. Pages are kept away from them by
`ns_attr_name_is_internal` (`src/dom.c:1353`), called from about 30 places
in the parser, the XML parser and the attribute bindings.

Thirty ways of setting an attribute were tried — `setAttribute` in every
variant, `Attr` nodes, `dataset`, every HTML and XML parsing entry point,
declarative shadow DOM, `document.write`, XHR documents, foreign content —
and none planted `data-nd-frame-url`. Only prefixed names such as
`x:data-nd-frame-url` are stored, and lookups do not match them.

It is still a deny-list in front of state that decides a frame's origin: one
new attribute-writing path that forgets the filter is a universal
cross-site-scripting bug. Moving these fields into `ns_node` (or a side
table keyed by node, as `js->frame_urls` already is) removes the class.

## 4. Checked and found sound

Each line was exercised against the built renderer unless marked *(read)*.

- **Cross-origin frames.** `contentDocument`, `contentWindow.document`,
  `eval`, `Function`, `fetch`, `localStorage`, `name`, `frameElement`,
  property descriptors and prototype access all throw `SecurityError`;
  `javascript:` assigned to a cross-origin frame's `location` or `src` does
  not run there; borrowed `setTimeout.call(frameWindow, …)` runs in the
  caller's realm. `BroadcastChannel`, `localStorage` and blob URLs do not
  cross origins. `history.pushState` to another origin throws.
- **CORS.** Responses without a matching `Access-Control-Allow-Origin` are
  refused, including prefix matches, lists, `null`, `*` with credentials,
  and a missing `Allow-Credentials`; redirects to another origin are
  refused; non-simple methods and headers are preflighted; response headers
  are filtered to the safelist; `Set-Cookie` is never exposed to `fetch` or
  XHR. Opaque responses have status 0, no URL and an empty body through
  `text()`, `clone()`, streams, the Cache API and a service worker.
- **Other cross-origin reads.** Canvas tainting holds for `drawImage`,
  patterns, canvas-to-canvas copies, `ImageBitmap`, `OffscreenCanvas`,
  `toBlob`, `convertToBlob`, WebGL `texImage2D`, and SVG images that embed
  remote images or `foreignObject`. Cross-origin style sheets expose no
  rules and a `text/plain` response is not applied as CSS. `<track>` cues,
  JSON and CSS module imports, `EventSource`, fonts, `<object>`/`<embed>`
  documents, external `<use>` and Resource Timing sizes leak nothing.
  `X-Content-Type-Options: nosniff` blocks mistyped scripts.
- **Cookies.** `HttpOnly` cookies are invisible to script and cannot be
  overwritten or deleted by it; `__Host-` and `__Secure-` prefixes and
  `Domain` mismatches are enforced; control characters are rejected.
- **CSP** (apart from F2–F4). Inline scripts, event handlers, `javascript:`
  URLs, `eval`, `Function`, string timers, `data:`/`blob:` script elements,
  dynamically inserted scripts, `srcdoc` and `about:blank` child frames,
  `base-uri`, a late `<meta>` relaxing the policy, and `connect-src`,
  `img-src`, prefetch and preload (no request reached the other origin).
- **Sandboxed and opaque frames.** No script without `allow-scripts`
  (including handlers and `javascript:` links); with it, an opaque origin,
  no cookies or storage, no parent access, no top navigation, popups or
  form submission.
- **Privileged schemes.** Web content cannot fetch, frame, redirect to,
  navigate to or submit forms to `about:history`, `about:settings`, `view-source:` or
  `file:`.
- **`file:` pages.** Opaque origin; `fetch`/XHR of sibling files is refused;
  anything outside the page's own directory tree is refused, including
  encoded traversal; images taint the canvas; frames of other files cannot
  be read.
- **Request integrity.** The internal `X-ND-*` control headers cannot be set
  through `fetch` (object, array, `Headers`, `Request`) or XHR; CR/LF in
  header names, values, methods, URLs, Blob types and FormData names does
  not split the request; `TRACE` and `CONNECT` are refused.
- **Buffer handling** *(read and run)*. Every binding that takes an
  ArrayBuffer view re-derives pointer and length after the last
  script-visible conversion and checks `offset + length <= total`
  (`ns_js_bytes_view`, `ns_wc_dup_buf`, `putImageData`, `ImageData`
  construction, WebAudio, WebCrypto).
- **Decoders and caches** *(read)*. `src/image_webp.c` bounds dimensions,
  frame count and total bytes before allocating; `src/content_decode.c` caps
  every stage's output and the stage count; the WebVTT parser saturates its
  integers; `src/cache.c` keys entries by URL and partition, refuses
  `Vary` on anything but four safe headers and stores bodies under hashed
  names.
- **Local services** *(read)*. WebDriver refuses any request carrying an
  `Origin` header or a non-loopback `Host`, and form content types; the
  shell forwards only `open`/`reload` audio commands whose path is inside
  the audio cache, with the same tokenisation the helper uses.

## 5. GTK shell review

A second pass looked at the GTK shell alone: what a web page can make it
do, and how it treats what a renderer tells it.

### Method

The shell was built with ASan + UBSan in the same scratch tree and run on a
virtual X display (Xvfb, Cairo renderer, isolated `$HOME`), driven with
synthetic mouse and keyboard input and inspected through screenshots. It ran
as root with `NS_NO_SANDBOX=1`, so Landlock and seccomp were not in force.
No ASan report was produced in any shell run. Read by hand: the result
handler and side-channel consumers in `src/gtk/procview.c`, downloads,
popups, the settings relay and event handlers in `src/gtk/procwindow.c`, the
reply parsers in `src/rproc_http.c`, `src/gtk/rjson.c`, texture creation in
`src/gtk/pagelayers.c`, the remote-control start path, and the generated
history page.

Not covered: WebAuthn, printing, drag and drop, context-menu actions, the
MCP and remote-control servers at runtime, session-restore parsing, GPU
rendering paths, and everything specific to macOS and Windows.

### Findings

| # | Finding | Attacker | Severity | Status |
|---|---------|----------|----------|--------|
| G1 | Downloads start with no user gesture and no prompt, without limit | any web page | medium | fixed after this review |
| G2 | Download names may start with a dot | any web page | medium where the download folder is `$HOME`, else low | fixed after this review |
| G3 | Two downloads of the same name in flight overwrite each other | any web page | low | by reading |
| G4 | The program-file warning does not cover local active content (`.html`, `.svg`) | any web page | low | by reading |
| G5 | Renderer-supplied numbers converted without range checks; request JSON built with unchecked `snprintf` chaining | compromised renderer | low (hardening) | by reading |
| G6 | The shell still takes a tab's URL from the renderer | compromised renderer | high (unchanged S1) | by reading |
| G7 | Remote control still starts from a configuration-file change | compromised renderer on macOS/Windows | high there (unchanged S3) | by reading |

**G1. Downloads need no gesture.** An anchor with a `download` attribute is
honoured from a synthetic `click()` (`src/js.c:51022-51026`), and the shell
starts a download for every `X-Download` offer when the site's `downloads`
permission is at its default, `allow` (`src/gtk/procwindow.c:2012-2033`).
Confirmed: a page with a timer saved twelve files into the download folder
in eight seconds with no interaction; the only visible effect is that the
Downloads window opens. Other browsers allow one automatic download per page
and ask before the next.

Fix: carry the user-activation bit with the offer, as navigation already
does (`X-ND-User-Activated`); let one offer per page load through without a
gesture and move the site to `ask` for further ones. As the shell cannot
trust that bit from a compromised renderer, also rate-limit offers per tab
in the shell.

**G2. Hidden file names.** `download_sanitize_name`
(`src/gtk/procwindow.c:1392-1427`) strips path separators, control and bidi
characters, trailing dots and Windows device names, but keeps a leading dot.
Confirmed: the page above saved `.bash_aliases` into the download folder
silently. In the default `~/Downloads` that is only a hidden file. Where the
download folder is the home directory itself, a name such as
`.bash_aliases` or `.xprofile` that does not exist yet becomes a file the
user's shell or session runs — code execution from visiting a page, when
combined with G1.

Fix: replace leading dots with `_`, as other browsers do.

**G3. Name chosen at start, file moved at the end.** `pw_start_download`
picks a free name with `g_file_test` (`procwindow.c:1604-1611`) and the file
is renamed into place when the transfer finishes (`download_move_posix`,
`:1210-1244`), replacing whatever is there. Two downloads of the same name
started before either finishes get the same path, and the second replaces
the first. Sequential downloads were confirmed to get `name.1`.

Fix: reserve the name when the download starts (create it with `O_EXCL`),
or re-check with `renameat2(RENAME_NOREPLACE)` at the end.

**G4. Warning list.** `download_name_is_executable` is a deny-list of about
120 extensions. It covers programs and installers but not documents that
run script with local-file privileges when opened from the Downloads
window's Open button: `.html`, `.htm`, `.xhtml`, `.svg`. With F1 fixed such
a page can no longer read its neighbours through script errors, so this is
low; it is listed because a deny-list will keep missing types.

**G5. Unchecked conversions and string building.**
`on_download_polled` casts the renderer's `bytes` and `total` doubles to
`guint64`/`gint64` (`procwindow.c:1300-1311`), which is undefined for NaN or
negative values. `render_request_json` (`src/rproc_http.c:763-794`) appends
to a 320-byte stack buffer with `jn += snprintf(json + jn, cap - jn, …)`
and never checks `jn` against `cap`; with every integer at its longest the
text is about 316 bytes, so it fits today by four bytes, and one more field
would make `cap - jn` wrap. Clamp the doubles and build the request with a
bounded appender.

**G6 and G7** are S1 and S3 of the earlier report and are unchanged:
`on_result` stores `res->url` from the renderer as the tab's URL
(`src/gtk/procview.c:3808-3809`), and that value still decides the
`history_clear` gate (`:3738`), the origin used for permission and download
decisions, and the URL bar; `rc_on_config_changed`
(`src/gtk/remotectl.c:1010-1028`) still starts the remote-control server
when the configuration file changes on disk. In addition, any renderer can
raise the "Apply these security-related settings?" dialog at any time,
whatever page it shows (`procview.c:3859-3860`); the dialog is correctly
default-cancel, so this is a nuisance rather than a bypass.

### Checked and found sound

Observed in the running shell:

- **URL bar.** User information is stripped (`http://user:pass@host/` shows
  the host only), percent-escapes are not decoded (an encoded right-to-left
  override stays as `%E2%80%AE`), and IDN hosts are shown as punycode. A
  page-initiated navigation does not change the bar until it commits.
- **Popups.** Blocked without a click; one click opens one tab however many
  `window.open` calls it makes. A cross-origin popup sees a non-null
  `opener` but can neither read it nor navigate it.
- **Dialogs.** `alert`, `confirm` and `prompt` draw no browser UI at all
  (`prompt` returns `null`), so a page cannot imitate a browser or
  cross-site dialog with them.
- **Fullscreen.** Refused without a click; on entry a notice names the host
  and Esc leaves.
- **Clipboard.** Writing needs a click; reading is denied.
- **Permissions.** The camera prompt names the page's origin. (By reading:
  a request for any origin other than the tab's is refused by the shell.)
- **Program downloads.** `setup.exe` raised "Save this program file?" with
  Cancel as the default.
- **Profile.** Directories are `0700` and files `0600`; private tabs are not
  written to the session store.

By reading:

- Frame, tile, favicon and layer geometry from the renderer is range-checked
  against the shared-memory size before any pixel is read
  (`src/rproc_http.c:827-846`, `:2210-2216`, `src/gtk/pagelayers.c:133-146`).
- The JSON parser used on renderer data has a depth limit of 64.
- A finished download is moved only if it is a regular file directly inside
  the staging directory, opened with `O_NOFOLLOW` and re-checked by inode.
- Renderer-requested settings pass an allow-list of fifteen keys and all
  but one of those (`images_enabled`) prompt; cross-window `call` and `post` relays check the
  claimed origin against the sender's tab and are re-checked by the
  receiving renderer.
- The shell launches no external program for a page; only the Open button of
  a finished download does. Titles and URLs are escaped in the generated
  history and start pages.

## 6. Suggested order of work

1. F1 and F2 — done.
2. G1 and G2 — done.
3. F3 and F4 — two one-line policy checks.
4. F5 — one more scheme in the navigation deny-list.
5. F6 — refuse wasm buffers in the transfer list.
6. F7 — one saturating conversion helper, then `-fwrapv`.
7. F8 — move frame URL and sandbox state out of DOM attributes.
8. An ASan + UBSan CI job (B3 of the earlier report). This review had to
   edit the build files to get one; a `-Dsanitize` profile that skips
   `_FORTIFY_SOURCE` and `exe-local.map` would make it routine.
