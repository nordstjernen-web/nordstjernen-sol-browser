# Nordstjernen full source review — security, bugs, HTML/CSS conformance

Tree reviewed: `nordstjernen-web/nordstjernen-sol-browser` at `a58ff38`
(identical to `nordstjernen-web/nordstjernen-browser` at the same commit),
release 1.0.44, November 2026.

This is a **static source reading**. No build was available in the review
environment (GTK 4 and libcurl absent), so no finding below was exercised
against a running browser. Every finding names the code path it rests on with
`file:line` references into this tree; items the reviewer could not pin down
end-to-end are marked **unconfirmed** in place. Severity follows the scale used
in `docs/cookies-security-report.md`: **High** means a remote page defeats a
boundary the web platform promises (same-origin, sandbox, CSP, process
isolation) or gains a reliable renderer crash with memory-safety character;
**Medium** means a boundary is weakened in a realistic setup or the browser
can be hung/killed by content; **Low** is a hardening gap or a correctness
problem with narrow consequence; **Info** is advisory.

Scope was the whole of `src/` outside the vendored upstream trees, plus the
local modifications *to* those trees (`src/quickjs`, `src/lexbor`,
`src/web-text-lib`, `src/wamr`), the bundled HTML/CSS the browser renders in
its own privileged pages, the packaging scripts and the `subprojects/` wraps.
The review was split into ten areas, each read in full by a separate reviewer
and cross-checked at the seams; the per-area reports follow the summary
unchanged, so each finding keeps its original ID (`NET-`, `HTML-`, `CSS-`,
`JSA-`, `JSB-`, `LAY-`, `GPU-`/`MED-`, `APP-`, `CONF-`, `VEN-`).

## Executive summary

| Area | Files | Critical | High | Medium | Low | Info | Total |
|---|---|---|---|---|---|---|---|
| Network, IPC, sandbox | `net.c`, `rproc_http.c`, `renderer_serve.c`, `security.c`, `csp.c`, `cookies.c`, `cache.c`, `ws.c`, `eventsource.c`, `gtk/procview.c`, `gtk/remotectl.c`, `gtk/mcp.c` … | 0 | 1 | 3 | 8 | 2 | 14 |
| HTML, DOM, SVG, images, fonts, forms | `html.c`, `html_lexbor.c`, `dom.c`, `xml.c`, `svg.c`, `mathml.c`, `image*.c`, `font.c`, `woff2.c`, `forms.c` … | 0 | 0 | 6 | 12 | 4 | 22 |
| CSS engine | `css.c`, `css_*.c` | 0 | 0 | 3 | 12 | 2 | 17 |
| JS/DOM binding (js.c 1–36500) | `js.c` | 0 | 2 | 1 | 2 | 0 | 5 |
| JS Web APIs (js.c 36500–end, canvas, WebCrypto, IDB, wasm, extensions) | `js.c`, `js_canvas*.c`, `webcrypto.c`, `idb.c`, `wasm.c`, `ext.c` … | 0 | 2 | 2 | 5 | 1 | 10 |
| Layout, paint, animation | `layout.c`, `paint.c`, `anim.c`, `render.c`, `engine.c` | 0 | 1 | 3 | 3 | 1 | 8 |
| WebGL, WebGPU, media | `webgl.c`, `webgpu.c`, `glctx.c`, `video*.c`, `mp4.c`, `media_os*.c`, `audio/main.c` | 0 | 3 | 4 | 5 | 3 | 15 |
| Shell, persistence, embedding | `gtk/*.c`, `libnordstjernen.c`, `headless.c`, `config.c`, `history.c`, `win_launcher.c` … | 0 | 1 | 1 | 6 | 5 | 13 |
| Bundled pages and parser conformance | `about:` / error pages, `css_syntax.c`, `css_grammar.c`, selector parser, tree-builder consumption | 0 | 0 | 3 | 14 | 3 | 20 |
| Vendored-engine modifications | `quickjs/` fork, `lexbor/` fork, `web-text-lib/` shape cache, `wasm.c`, wraps | 0 | 0 | 1 | 7 | 5 | 13 |
| **Total** | | **0** | **10** | **27** | **74** | **26** | **137** |

### Overall assessment

The codebase is in markedly better shape than its size (about 245k lines of
hand-written C) would suggest. No Critical finding was made, and **no
classic memory-corruption bug (stack/heap overflow, format string, unchecked
length-field copy) was found in any parser** — image decoders, WOFF2, MP4,
the CSS tokenizer and the DOM walkers all carry explicit bounds checks, depth
caps and byte budgets. Privileged `about:` and error pages escape every piece
of untrusted data they interpolate; there is no XSS into browser chrome. SQL
is parameterised everywhere. The sandbox (Landlock plus seccomp-bpf) is
genuinely restrictive and the shell re-validates everything the renderer sends
it. Supply-chain hygiene is good: every wrap pins a hash and every download
script verifies SHA-256.

The ten High findings cluster into four themes rather than ten independent
bugs:

1. **Boundary checks performed against the wrong principal.** Streaming XHR
   checks CORS against the requested URL rather than the redirect target
   (JSA-1); IndexedDB partitions on the top-level origin rather than the
   realm, so a cross-origin iframe reaches the embedder's databases (JSB-1);
   `window.open` skips the navigation-policy check that `location`
   assignment goes through, so one click opens `file:///` or
   `about:settings` from the web (APP-1); a page-triggered download is
   re-fetched as a first-party request with SameSite=Strict cookies (NET-2).
2. **State keyed by raw pointers that outlive the thing they point to.**
   Canvas 2D state survives navigation and is keyed by `ns_node *`, so a
   recycled address hands the next page the previous page's pixels and a
   freed `JSContext` (JSB-2); WebGL/WebGPU contexts, animation state and
   `canvas_states` share the pattern (GPU-7, LAY-8).
3. **Re-entrancy during a blocking operation.** Synchronous XHR pumps the
   GLib main loop without the `in_pump` guard that the rest of the engine
   uses, so an async XHR completing underneath frees state still on the
   stack (JSA-2, JSA-3).
4. **Trusting the GPU library to be tolerant.** WebGL forwards
   `vertexAttribPointer` with no array buffer bound and lets program-less
   draws through the vertex-range check, which on the Windows
   compatibility-profile WGL context means fixed-function reads from a
   page-chosen host address (GPU-1); WebGPU reaches two documented
   wgpu-native abort paths from page-controlled input (GPU-2, GPU-3).

The one architectural High (NET-1) is already documented in
`docs/tab-isolation.md` as remaining work: every renderer's Landlock ruleset
grants read-write on the shared profile (all-site cookies, the HTTP cache
whose keys are predictable, HSTS, storage), so one compromised tab is every
site. The sandbox is only as strong as that broker-less design allows.

On the **HTML/CSS conformance** side the engine is close to spec in the places
that matter for real pages; the deviations found are concentrated in a few
subsystems and most are cheap to fix: the component-value tokenizer lacks a
`url-token` (CONF-9), legacy charset handling abandons a declared encoding on
one bad byte and prescans with a substring search (HTML-1..4, CONF-15), scripts
run after the whole document is built so `document.write` is emulated rather
than streamed (CONF-16), native form submission ignores `enctype` and drops
file controls (HTML-7), explicit grids are capped at 24 tracks (LAY-4), and the
`An+B`, `:not()`, `calc()` whitespace and `round()` tie rules have small
divergences. The bundled privileged pages are valid apart from missing
`<html lang>`, one inline `onclick`, and an `alt`/`aria-hidden` contradiction.

Two documentation corrections fall out of the review: `CLAUDE.md` says the
QuickJS fork carries a JIT and `SECURITY.md` lists JIT as not implemented;
there is no JIT in the tree (VEN-9). And `docs/HTML-compatibility.md` claims
native multipart form submission, which does not exist (HTML-7).

## Priority fix list

Ordered by the reviewers' judgement of exploitability and blast radius. Each
entry names the full finding below.

| # | ID | Sev | One-line description | Where |
|---|---|---|---|---|
| 1 | APP-1 | High | `window.open()` emits the popup URL without `browser_allows_navigation_url`; the shell opens any scheme; the fresh renderer has no referrer so `file:`/`about:settings` loads count as chrome-initiated. | `src/js.c:27638`, `src/gtk/procwindow.c:1815`, `src/net.c:4515`, `src/net.c:5169` |
| 2 | JSB-1 | High | IndexedDB partition comes from `js->partition_key` (top-level origin), not the realm; the polyfill's `IDBFactory.prototype.open` is reachable from cross-origin and sandboxed frames. | `src/idb.c:118`, `src/js.c:72948`, `src/js.c:53452`, `data/js/polyfills.js:137` |
| 3 | JSB-2 | High | `js->canvas_states` keyed by raw `ns_node *`, never cleared on navigation; recycled address yields cross-document pixels and a freed `JSContext`. | `src/js.c:66816`, `src/js.c:65956`, `src/js_canvas.c:698` |
| 4 | JSA-2 | High | Sync XHR pumps the main loop without `in_pump`; re-entrant completion frees `ns_xhr_state` under `ns_xhr_on_chunk`. | `src/js.c:22304`, `src/js.c:22147`, `src/js.c:22116` |
| 5 | JSA-1 | High | Streaming XHR validates CORS against the requested URL; redirected cross-origin bodies and headers reach script at readyState 2/3. | `src/js.c:22077`, `src/net.c:7211`, `src/net.c:6672` |
| 6 | GPU-1 | High | `vertexAttribPointer` accepted with no `ARRAY_BUFFER` bound; `wgl_attribs_cover` validates nothing when no program is current; WGL context has no core-profile mask. | `src/webgl.c:2694`, `src/webgl.c:850`, `src/webgl.c:925`, `src/glctx.c:88` |
| 7 | GPU-2 | High | Canvas readback submits an unvalidated command buffer; `wgpuQueueSubmit` abort path reachable via destroyed current texture, bad `viewFormats`, small `maxBufferSize`, `device.destroy()`. | `src/webgpu.c:4932`, `src/webgpu.c:2344` |
| 8 | GPU-3 | High | Error buffer from invalid `createBuffer` descriptor still marked mapped; `getMappedRange` reaches fatal `wgpuBufferGetMappedRange`. | `src/webgpu.c:876`, `src/webgpu.c:767` |
| 9 | LAY-1 | High | Nested `display:inline-block` containers lay out in O(3^depth); ~20 levels hang the renderer. | `src/layout.c:10118`, `src/layout.c:10575`, `src/layout.c:7676` |
| 10 | NET-1 | High | Every renderer's Landlock ruleset grants read-write on the shared profile (cookies, HTTP cache, HSTS, storage, Downloads). Architectural; documented as remaining work. | `src/security.c:556-588` |
| 11 | NET-2 | Medium | Download re-fetch uses `top_url == target`: SameSite=Strict cookies, `Sec-Fetch-Site: same-origin`, private-network guard bypassed. GET-CSRF with credentials. | `src/renderer_serve.c:1309`, `src/net.c:6373` |
| 12 | VEN-1 | Medium | Worker runtimes never call `JS_SetCanCompileStrings`; `eval`/`new Function` bypass `script-src` in workers. One-line fix. | `src/js.c:30117`, `src/js.c:62929` |
| 13 | APP-2 | Medium | Popup/opener `postMessage` origin copied verbatim from the sending renderer; a compromised renderer forges `event.origin` into another process. | `src/gtk/procwindow.c:1772`, `src/gtk/procwindow.c:1836` |
| 14 | JSB-3 | Medium | Uncaught errors from non-CORS cross-origin scripts reach `window.onerror` unmuted (XS-Leak). | `src/js.c:67240`, `src/js.c:67828` |
| 15 | CSS-1 | Medium | `:visited` styled from a process-global set with no history-leak mitigation; iframe `src` loads populate it. | `src/css.c:25204`, `src/css.c:26197`, `src/js.c:70284` |
| 16 | NET-3 | Medium | Address-space cache keyed by requested origin is poisonable by DNS rebinding from a private page. | `src/net.c:2133-2168` |
| 17 | NET-4 | Medium | seccomp leaves `rt_sigqueueinfo`, `rt_tgsigqueueinfo`, `prlimit64`, scheduler calls unfiltered; renderer can SIGKILL the shell. | `src/security.c:838-869`, `src/security.c:1005` |
| 18 | GPU-4 | Medium | `onSubmittedWorkDone` leaves a wgpu callback pointing at a dead stack frame after 4000 polls. | `src/webgpu.c:582-607` |
| 19 | HTML-5 | Medium | Nested SVG opacity/mask/filter groups allocate a full-size surface per level; ~200 nested `<g opacity>` in 10 KB requests ~12 GB. | `src/svg.c:2803`, `src/svg.c:1970` |
| 20 | LAY-2 / LAY-3 | Medium | Non-finite CSS lengths reach layout unclamped; `(int)(x * NS_PANGO_SCALE)` casts are UB above ~2.1e6 px. | `src/css.c:3101`, `src/layout.c:27`, `src/layout.c:8287`, `src/paint.c:4001` |
| 21 | CSS-2 | Medium | `:has()` descendant walks charge nothing to the selector-match budget. | `src/css.c:25797-25842` |
| 22 | JSB-4 | Medium | Extension content scripts run in the page realm, not an isolated world. | `src/js.c:72057`, `src/ext.c:1174` |

Conformance fixes with the best return (all Medium, none security):
CONF-9 url-token in `css_syntax.c`; CONF-15/HTML-1..4 charset prescan and
U+FFFD replacement using the existing `ns_encoding_labels` table; HTML-7
multipart submission; CSS-3 stylesheet re-parse on changed bytes; LAY-4 grid
track cap; the WPT clusters mapped in the conformance section (innerText,
`dir=auto` with slots, `Attr`/`NamedNodeMap`, UTF-16 `CharacterData`).

## Status of the High findings (2026-11-06)

Each High finding got its own pull request, built on Linux, macOS and Windows
by CI and, where the review container allowed it, reproduced headless before
and after the change.

| ID | Pull request | Outcome |
|---|---|---|
| APP-1 | [#919](https://github.com/nordstjernen-web/nordstjernen-sol-browser/pull/919) | Fixed: popup URLs go through the navigation policy, the shell allow-lists popup schemes, and the opener origin becomes the load's initiator. |
| JSB-1 | [#922](https://github.com/nordstjernen-web/nordstjernen-sol-browser/pull/922) | Hardened. **Severity corrected to Medium**: a real cross-origin frame never receives `IDBFactory` or the backend, because the frame's `indexedDB` deny getter is installed before the polyfill, which then skips its IndexedDB section. The partition is now realm-aware regardless. |
| JSB-2 | [#923](https://github.com/nordstjernen-web/nordstjernen-sol-browser/pull/923) | Fixed: canvas state dropped on navigation and node removal; re-initialisation uses the main realm's context. |
| JSA-2 / JSA-3 | [#924](https://github.com/nordstjernen-web/nordstjernen-sol-browser/pull/924) | Fixed: sync XHR holds page tasks; only its own completion is delivered; delivery deferred while a chunk handler is on the stack. |
| JSA-1 | [#926](https://github.com/nordstjernen-web/nordstjernen-sol-browser/pull/926) | Fixed and reproduced: the unfixed build exposed 2400 body bytes and 122 header bytes of a cross-origin redirect target at readyState 3; the fixed build exposes nothing. |
| GPU-1 | [#928](https://github.com/nordstjernen-web/nordstjernen-sol-browser/pull/928) | Fixed: attribute pointers need a bound array buffer, draws need a program. The Windows context profile is unchanged (a core profile would need a default VAO). |
| GPU-2 / GPU-3 | [#929](https://github.com/nordstjernen-web/nordstjernen-sol-browser/pull/929) | Fixed: buffer, target and readback creation and the readback copy run inside a validation scope. |
| LAY-1 | [#930](https://github.com/nordstjernen-web/nordstjernen-sol-browser/pull/930) | Fixed and reproduced: 12 nested inline-blocks took 50 s and 16 never finished; after the change 40 levels take 0.14 s, with identical layout dumps for all bundled render tests. |
| NET-1 | none | Not fixed. Giving renderers a network/storage broker so they stop holding the profile is the multi-step architectural change `docs/tab-isolation.md` already describes; it is not a single pull request. |

## Cross-cutting recommendations

- **Make the realm, not the process, the unit of trust for storage and
  policy.** `ns_js_storage_partition`, `canvas_states`, `g_visited_urls` and
  the error-event muting all consult process- or top-level state where the
  spec wants the realm's origin. A single `ns_realm_origin(ctx)` accessor used
  by every storage/policy entry point would close JSB-1, JSB-3, CSS-1 and the
  worker CSP gap (VEN-1) together.
- **Replace raw `ns_node *` hash keys with invalidation-aware handles.** The
  DOM already has `ns_node.js_invalidate`; the canvas, WebGL/WebGPU and
  animation tables should hook it (JSB-2, GPU-7, LAY-8) and be cleared in
  `ns_js_reset_runtime_state`.
- **Route every nested main-loop pump through one guarded helper** that sets
  `in_pump`, so sync XHR behaves like `alert()`/`confirm()` (JSA-2, JSA-3).
- **Treat wgpu-native and the GL driver as hostile to invalid input.** Wrap
  every submit and mapping in the existing internal validation scope, reject
  WebGL calls the spec says must fail before they reach GL, and request a
  core profile on Windows (GPU-1..5).
- **Carry the initiator through downloads and popups** so `top_url`, the
  `Sec-Fetch-*` headers and the navigation-policy check see the real opener
  (NET-2, APP-1, APP-2).
- **Finish the network/storage broker** described in `docs/tab-isolation.md`.
  Until then NET-1 bounds what every other fix buys.
- **Fix the documentation drift** (no JIT; no native multipart) so the threat
  model in `SECURITY.md` and the capability list in
  `docs/HTML-compatibility.md` match the tree.

## Duplicates across areas

Independent reviewers converged on the same code from different angles; the
pairs below describe one bug each: HTML-4 and CONF-15 (charset prescan);
HTML-1 and CONF-15 (legacy decode fallback); HTML-11 and CONF-18 (whitespace
text node pruned under `<html>`); HTML-19 and CONF-17 (`<script` line
numbers from a text scan); CSS-12 and CONF-13 (`An+B` whitespace); APP-3 and
MED-1 (lexical path checks vs. symlinks, two different directories); VEN-3
and NET-4/NET-11 (seccomp breadth); LAY-8, GPU-7 and JSB-2 (raw node-pointer
keys).

---

# Area reports

Each section below is the unabridged per-area report.

## Nordstjernen — network / IPC / sandbox security review

Scope: `src/net.c`, `src/net.h`, `src/rproc_http.c`, `src/renderer_http.c`,
`src/ipc_http.c`, `src/renderer_serve.c`, `src/renderer_tiles.c`,
`src/security.c`, `src/csp.c`, `src/cookies.c`, `src/cache.c`,
`src/content_decode.c`, `src/download.c`, `src/ws.c`, `src/eventsource.c`,
`src/watchdog.c`, `src/engine_protocol.h`, `src/gtk/procview.c`,
`src/gtk/rjson.c`, `src/gtk/remotectl.c`, `src/gtk/mcp.c`, plus the
Landlock/seccomp code in `src/security.c` and the shell-side consumers of
renderer data in `src/gtk/procwindow.c` and `src/libnordstjernen.c` that
were needed to confirm findings. Static review only (no build possible);
HEAD a58ff38. Every finding below was confirmed by reading the callers and
the checks done elsewhere; anything not confirmable is marked
"unconfirmed".

Severity scale: Critical / High / Medium / Low / Info.

---

### NET-1 — Every renderer has read-write access to the whole shared profile (cookies, HTTP cache, HSTS, storage)

- Severity: **High** · Category: security (architectural)
- `src/security.c:556-588`, `src/cookies.c` (store path from `src/net.c:1458-1470`), `src/cache.c:302-306`

```c
    if (!ns_config_dir_writable) {
        g_autofree char *cookies =
            g_build_filename(ns_cfg_root, "cookies", NULL);
        g_mkdir_with_parents(cookies, 0700);
        add_path_rw(rfd, fs_rw, cookies);
    }
    ...
    add_path_rw(rfd, fs_rw, ns_data_root);      /* ~/.local/share/nordstjernen */
    ...
    add_path_rw(rfd, fs_rw, ns_cache_top);      /* ~/.cache/nordstjernen       */
    add_path_rw(rfd, fs_rw, ns_cache_root);     /* …/cache (HTTP cache)        */
```

Trigger: a memory-safety bug in the engine gives an attacker code execution
inside one tab's renderer. That renderer's Landlock ruleset still grants
read+write on `~/.config/nordstjernen/cookies/cookies.db` (all sites'
cookies, `cookies.c` opens it directly), on the SQLite HTTP cache plus its
body files (`cache.c`, keys are `SHA256(url \x1f partition)` and therefore
computable for any victim URL, and the row carries `cors_allow_origin` /
`policy_headers` that other renderers trust), on `hsts-curl.txt`,
`altsvc.txt`, `localstorage/`, `indexeddb/` and on `~/Downloads`.

Consequence: process-per-tab does not isolate *sites* from each other once
a renderer is compromised: cookie theft for every site, planting a cached
`200` script body for any URL another tab will load (persistent cross-site
code injection), HSTS/Alt-Svc manipulation, overwriting downloaded files.
`docs/tab-isolation.md` lists "browser-process services for networking,
cookies, cache and storage" as remaining work, so this is a known gap, but
it is the single largest hole in the sandbox and the review would be
incomplete without it.

Fix: move the cookie DB, HTTP cache, HSTS/Alt-Svc files and site storage
behind the shell (or a dedicated unsandboxed network service) and hand the
renderer only its own site's data over the IPC channel; as an interim step
give each renderer a per-process cache directory and open the cookie DB
read-only in the renderer with writes proxied through `/render` headers.

---

### NET-2 — Download re-fetch runs as a first-party request: SameSite=Strict cookies and the private-network guard are bypassed

- Severity: **Medium** · Category: security
- `src/renderer_serve.c:1309`, `src/download.c:176`, `src/net.c:6373`, `src/net.c:6417-6431`, `src/net.c:6704`

```c
    if (strcmp(head->path, "/download-start") == 0) {
        char *url = json_get_str(body, "url");
        long id = url && *url ? ns_download_start(url, url) : 0;      /* top_url = url */
```
```c
    const char *effective_top_url = is_navigation || !top_url ? url : top_url;
    ...
    const char *partition_url = top_level_url ? top_level_url : effective_top_url;
    ...
        .document_url = is_navigation ? NULL : top_url,
```

Trigger: a page on `https://evil.example` offers a download whose target is
`http://192.168.1.1/cgi-bin/reboot` or `https://bank.example/transfer?to=…`
(`browser_js_download`, `src/libnordstjernen.c:968-983`, accepts any
http(s) URL). The shell receives it as `X-Download`, checks only the scheme
and the `downloads` capability (default *allow*), then asks the renderer to
`/download-start`, which calls `ns_download_start(url, url)`. In
`ns_fetch_sync_hop` the top URL therefore *is* the target: the cookie
partition is the target's own site, `cross_site_request` is false so
`samesite_cross == 0` and **SameSite=Strict/Lax cookies are attached**,
`Sec-Fetch-Site: same-origin` and a same-origin `Referer` are sent, and
`document_url == url` makes `ns_address_space_of_document()` return the
*target's* address space, so the loopback/RFC1918 guard never fires.

Consequence: GET-based CSRF with full first-party credentials against any
site, and a way around the private-network-access guard for GET requests to
routers / local services, needing only that the user accepts (or has
allowed) downloads. The downloaded body is written to disk, so the attacker
cannot read it, but state-changing GETs succeed.

Fix: carry the initiating document URL through `X-Download` →
`ns_rproc_http_download_start` → `/download-start` → `ns_download_start(url,
page_url)`, treat the fetch as a cross-site non-navigation request (so the
SameSite logic and the address guard use the real initiator), and mark it
with an `X-ND-Fetch-Dest: download` header so it does not inherit
first-party semantics by accident.

---

### NET-3 — Per-origin address-space cache can be poisoned by DNS rebinding from a more private context

- Severity: **Medium** · Category: security
- `src/net.c:2133-2149` (`ns_address_space_of_document`), `src/net.c:2152-2168`, `src/net.c:5970`

```c
    gpointer seen = g_address_space_by_origin
        ? g_hash_table_lookup(g_address_space_by_origin, origin) : NULL;
    g_mutex_unlock(&g_address_space_lock);
    if (seen) return (ns_address_space)(GPOINTER_TO_INT(seen) - 1);
    ...
        if (rc == CURLE_OK && out->remote_ip && !(req->proxy && *req->proxy))
            ns_net_record_address_space(req->url, out->remote_ip);
```

Trigger: the user has a page from a private/local context open (an intranet
app, a `localhost:3000` dev server, a router UI) that embeds any third-party
resource, e.g. `http://cdn.evil.example/a.js`. The attacker's DNS answers
`10.0.0.1` for that request. The guard allows it (private page → private
target) and `ns_net_record_address_space()` records
`http://cdn.evil.example` → PRIVATE, keyed by the *requested origin*, not
by what the document was. The recorded value takes precedence over the
host/IP classification for every later lookup. If the same tab is then
navigated to `http://cdn.evil.example/` (the private page can do that with
`window.location`), the attacker's top-level document is classified PRIVATE
and may freely connect to RFC1918/loopback addresses for the rest of the
renderer's life.

Consequence: bypass of the private-network guard that `NET-2` also touches;
LAN/loopback port scanning and request forgery from a public site. The
entry survives until the renderer process exits.

Fix: record the address space of a *document* only from the navigation
response that created it (and never from subresource fetches), or key the
cache on `(document origin, remote IP)` and let a literal/private
classification of the host override a cached PUBLIC/PRIVATE value when the
new value is more restrictive. Skip recording entirely when an environment
proxy is in effect (today the proxy's IP would be recorded).

---

### NET-4 — Seccomp pins `kill`/`tgkill` to the renderer's own PID but leaves `rt_sigqueueinfo`, `rt_tgsigqueueinfo`, `prlimit64` and scheduler calls unfiltered

- Severity: **Medium** · Category: security
- `src/security.c:838`, `:864`, `:869`, `:877-880`, `:895` (allow-list) vs `:1005-1008` (filtered kill)

```c
    "prlimit64",
    ...
    "rt_sigqueueinfo",
    ...
    "rt_tgsigqueueinfo",
    ...
    "sched_setaffinity", "sched_setattr", "sched_setparam", "sched_setscheduler",
    ...
    (void)seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(kill), 1,
                           SCMP_A0(SCMP_CMP_EQ, self_pid, 0));
```

Trigger: a compromised renderer calls `rt_sigqueueinfo(shell_pid, SIGKILL,
&si)` (or `rt_tgsigqueueinfo`), which the default-`EPERM` filter allows
with no argument check, although the explicit `kill`/`tgkill` rules were
written to prevent exactly this. `prlimit64(shell_pid, RLIMIT_CPU, {0,0})`,
`sched_setscheduler(shell_pid, SCHED_IDLE…)`, `setpriority(…,19)` and
`sched_setaffinity` likewise accept a foreign PID of the same UID. Landlock
`LANDLOCK_SCOPE_SIGNAL` only blocks the signal path on ABI ≥ 6 kernels
(6.12+) and does nothing for prlimit/scheduler calls.

Consequence: a renderer can kill or freeze the GTK shell and every sibling
renderer (the watchdog gives up after 5 restarts in 60 s), defeating the
"a renderer crash is contained to its tab" property. Denial of service
only, but across the trust boundary the sandbox is meant to enforce.

Fix: drop `rt_sigqueueinfo`/`rt_tgsigqueueinfo` from the allow-list (or add
`SCMP_A0 == self_pid` rules like `kill`), restrict `prlimit64` to
`A0 == 0 || A0 == self_pid`, and remove the `sched_set*`/`setpriority`
calls or pin their PID argument to 0/self. Consider `SCOPE_SIGNAL` as
defence in depth only.

---

### NET-5 — CORS preflight (and EventSource) accept `Access-Control-Allow-Origin: *` for credentialed requests and never check `Allow-Credentials`

- Severity: **Low** · Category: security
- `src/net.c:7128-7134`, `src/eventsource.c:258-266`

```c
    } else if (!acao || (strcmp(acao, "*") != 0 &&
                         g_strcmp0(acao, origin) != 0)) {
        failure = g_strdup("CORS preflight: origin not allowed");
```
```c
    return p->allow_origin &&
           (strcmp(p->allow_origin, "*") == 0 ||
            (origin && strcmp(p->allow_origin, origin) == 0));
```

Trigger: a page on `https://a.example.com` issues a non-simple `fetch()`
(e.g. `PUT` with a JSON body) to `https://b.example.com`. Both are the same
*site*, so under the default first-party cookie policy cookies are
attached; the preflight passes when `b` answers `ACAO: *` even though the
request carries credentials, and `Access-Control-Allow-Credentials` is
never consulted in the preflight path. The same applies to `EventSource`
streams (cookies are applied via `ns_net_curl_apply_cookies`, the response
is accepted with `*`). The final response check in `js.c`
(`cors_allows_credentialed`) does look at `Allow-Credentials`, so the main
exposure is the preflight for same-site/cross-origin or "always" cookie
policy and the SSE stream body.

Consequence: credentialed cross-origin requests that the spec says must
fail at preflight are sent; servers that rely on "`*` is never
credentialed" could receive authenticated writes.

Fix: in `ns_fetch_cors_preflight` and `ns_es_cors_allows` require an exact
origin match plus `Access-Control-Allow-Credentials: true` whenever the
request will carry cookies (`cookies_allowed` in the hop), and reject `*`
in that case.

---

### NET-6 — URL userinfo is passed straight to libcurl, so any subresource URL can send HTTP Basic credentials

- Severity: **Low** · Category: security
- `src/net.c:5788`, `src/net.c:1212-1313` (userinfo preserved in `ns_url_parts`)

```c
    curl_easy_setopt(curl, CURLOPT_URL, req->url);
```

Trigger: `<img src="http://admin:admin@192.168.1.1/">` (navigation to a
private host is also allowed) or `fetch("https://user:pass@victim/")`. The
WHATWG URL parser keeps the userinfo, nothing strips it before the request,
and libcurl turns it into an `Authorization: Basic …` header. Browsers drop
credentials from subresource URLs ("block if request URL includes
credentials") and never send them automatically for `fetch()`.

Consequence: web content can brute-force Basic-auth protected intranet
devices (status codes are visible through `<img onload/onerror>`), or
force the browser to authenticate to an attacker host with credentials the
attacker chose (useful for login-CSRF style attacks). Also a phishing aid
in the address bar (`https://good.example@evil.example/`).

Fix: strip username/password from every URL in `ns_fetch_sync` (or set
`CURLOPT_URL` to a credential-less serialisation and ignore userinfo),
except for an explicit top-level navigation typed by the user.

---

### NET-7 — On architectures without `NS_SECCOMP_FILTER_ARGS` the renderer keeps unrestricted `socket`, `clone`, `kill` and the Unix-socket denial is silently a no-op

- Severity: **Low** · Category: security
- `src/security.c:208-209`, `src/security.c:1009-1017`, `src/security.c:216-229`

```c
#if defined(NS_HAVE_SECCOMP) && (defined(__x86_64__) || defined(__aarch64__))
#define NS_SECCOMP_FILTER_ARGS 1
#endif
...
#else
    (void)seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(clone), 0);
    ...
    (void)seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(socket), 0);
    (void)seccomp_rule_add(ctx, SCMP_ACT_ALLOW, SCMP_SYS(kill), 0);
```

Trigger: build and run on riscv64, ppc64le, armhf or i386 Linux. The
renderer calls `ns_security_seccomp_deny_unix_sockets()`
(`src/libnordstjernen.c:1196`) expecting AF_UNIX to be blocked, but the
function body is compiled out, `socket()` is allowed for every domain,
`clone()` with `CLONE_NEWUSER` is allowed, and `kill()` accepts any PID.
`about:nordstjernen` still reports seccomp as enforced.

Consequence: on those platforms a compromised renderer can connect to the
session D-Bus (`/run/user/<uid>/bus` is readable through the `/run` rule),
the X11/Wayland sockets and any other local Unix service, and signal any
same-UID process. This is a sandbox-strength regression that is invisible
to the user.

Fix: make `ns_security_seccomp_deny_unix_sockets()` and
`ns_security_seccomp_deny_exec_memory()` fail loudly (or refuse to start
with `NS_REQUIRE_SANDBOX`) when argument filtering is unavailable, and
extend `NS_SECCOMP_FILTER_ARGS` to every architecture libseccomp supports
(the `SCMP_A0` comparisons are not architecture-specific).

---

### NET-8 — Private-mode temp directory falls back to a predictable `/tmp` name created with a non-CSPRNG

- Severity: **Low** · Category: security
- `src/net.c:478-491`

```c
    g_private_root = g_dir_make_tmp("nordstjernen-private-XXXXXX", NULL);
    if (!g_private_root) {
        char *base = g_strdup_printf("nordstjernen-private-%u", g_random_int());
        g_private_root = g_build_filename(g_get_tmp_dir(), base, NULL);
        g_free(base);
        g_mkdir_with_parents(g_private_root, 0700);
    }
    g_chmod(g_private_root, 0700);
```

Trigger: `g_dir_make_tmp` fails (full `/tmp`, EMFILE, hardened `TMPDIR`).
The fallback name uses `g_random_int()` (seeded from time/PID, not a
CSPRNG) and `g_mkdir_with_parents`, which succeeds on a directory that
already exists; a local attacker who pre-creates
`/tmp/nordstjernen-private-<guess>` as their own directory (or a symlink to
one) receives the private-mode cookie DB and HSTS file (`0600` files, but
inside a directory they own, and `g_chmod` of the root follows the
symlink).

Consequence: private-browsing cookies readable/replaceable by another local
user in the rare fallback path.

Fix: use `ns_security_csprng_fill` for the suffix, create with
`mkdir(…, 0700)` and fail (fall back to in-memory only) if it already
exists; or drop the fallback and run private mode fully in memory when
`g_dir_make_tmp` fails.

---

### NET-9 — Response memory budget is per transfer, not per process

- Severity: **Low** · Category: security (DoS)
- `src/net.c:2518`, `src/net.c:2557-2563`, `src/net.c:7667-7668` (32 concurrent fetches, 6 per host)

```c
static guint64
ns_net_response_budget(void)
{
    guint64 avail = ns_net_available_memory_bytes();
    if (avail == 0) return NS_NET_RESPONSE_MIN_BUDGET;
    guint64 half = avail / 2;
    return half < NS_NET_RESPONSE_MIN_BUDGET ? NS_NET_RESPONSE_MIN_BUDGET : half;
}
```

Trigger: a page starts 32 `fetch()`es (6 hosts × 6, or 32 CNAMEs of the
same server) whose bodies stream slowly; each transfer independently
accepts up to half of `MemAvailable` (re-checked only every
`NS_NET_RESPONSE_RECHECK_BYTES`), and gzip/brotli expansion is capped by
the same per-transfer number. The `discard_body` download stream path is
unbounded except for the 16 GiB file limit.

Consequence: the renderer (and, through swap pressure, the desktop) can be
driven to OOM by one tab; the kill is contained to the tab but the
"half of available memory" promise is not kept.

Fix: keep a process-wide running total of in-flight response bytes and
charge each `ns_write_body` against it; cap any single body at a fixed
fraction (e.g. 512 MiB) in addition to the dynamic budget.

---

### NET-10 — TLS cipher list still offers non-PFS RSA key exchange and CBC suites

- Severity: **Low** · Category: security
- `src/net.c:2291-2297`

```c
    curl_easy_setopt(curl, CURLOPT_SSL_CIPHER_LIST,
        "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256:"
        ...
        "ECDHE-RSA-AES128-SHA:ECDHE-RSA-AES256-SHA:"
        "AES128-GCM-SHA256:AES256-GCM-SHA384:AES128-SHA:AES256-SHA");
```

Trigger: any TLS 1.2 server that prefers (or is downgraded by an active
attacker to) `AES128-SHA`/`AES256-SHA`/`AES*-GCM-SHA*` (static RSA) or the
CBC-SHA1 ECDHE suites.

Consequence: sessions without forward secrecy (recorded traffic decryptable
if the server key leaks) and exposure to CBC-padding side channels; modern
browsers no longer offer these.

Fix: drop the four static-RSA suites and the two `ECDHE-RSA-AES*-SHA`
CBC suites; keep the GCM/CHACHA20 ECDHE set.

---

### NET-11 — Renderer can read all of `/proc`, `/sys` and `/run`

- Severity: **Low** · Category: security
- `src/security.c:500-507`

```c
    static const char *const system_read_dirs[] = {
        "/etc", "/var/lib/ca-certificates", "/var/cache/fontconfig",
        "/proc", "/sys", "/run",
        "/dev/shm", "/dev/dri",
```

Trigger: a compromised renderer reads `/proc/<shell pid>/environ`,
`/proc/<pid>/cmdline`, `/proc/net/*`, `/proc/<pid>/maps` of same-UID
processes (readable without ptrace permission), `/run/user/<uid>/*` and
`/sys/class/net/*/address`.

Consequence: information disclosure useful for fingerprinting the user
(MAC addresses, logged-in session details, other processes' environment
including tokens such as `SSH_AUTH_SOCK`, `DBUS_SESSION_BUS_ADDRESS`,
sometimes API keys exported in the shell). Not an escape by itself, but the
renderer needs only `/proc/self`, `/proc/meminfo`, `/proc/cpuinfo` and
`/sys/devices` for the GPU.

Fix: replace the three broad rules with `/proc/self`, `/proc/meminfo`,
`/proc/cpuinfo`, `/sys/devices`, `/sys/dev` and the specific `/run`
sub-paths the renderer actually opens; call `prctl(PR_SET_DUMPABLE, 0)` in
the shell so `/proc/<shell>/mem` and `environ` are not readable by the
same UID.

---

### NET-12 — `/print` writes sheets into `$XDG_RUNTIME_DIR`, which the renderer's Landlock rule makes read-only (unconfirmed)

- Severity: **Low** · Category: bug
- `src/rproc_http.c:1876-1878`, `src/renderer_serve.c:1577`, `src/libnordstjernen.c:1191`, `src/security.c:546`

```c
    char *prefix = g_strdup_printf("%s/nordstjernen-print-%" G_GINT64_FORMAT
                                   "-%d", g_get_user_runtime_dir(),
                                   g_get_monotonic_time(), ++counter);
```
```c
    ns_security_runtime_dir_read_only();          /* renderer */
    ...
    add_path_rw(rfd, ns_runtime_dir_writable ? fs_rw : fs_read,
                g_get_user_runtime_dir());
```

Trigger: File → Print in a sandboxed renderer on Linux. The shell asks the
renderer to rasterise into `$XDG_RUNTIME_DIR/nordstjernen-print-*.png`, but
`ns_browser_sandbox()` marks the runtime dir read-only before Landlock is
sealed, so `cairo_surface_write_to_png()` should fail with `EACCES` and the
shell gets `pages: 0`. Marked unconfirmed because it could not be run here
and `docs/printing.md` describes the same directory as the intended
location; if printing works today the Landlock rule is not doing what the
code says.

Consequence: printing silently produces nothing (or, if it does work, the
read-only intent of the rule is not enforced).

Fix: write print and export rasters under
`~/.cache/nordstjernen/print/` (already read-write for the renderer) and
have the shell read them from there, or add that one directory as writable
instead of the whole runtime dir.

---

### NET-13 — Remote-control listener lets unauthenticated local clients occupy all connection slots

- Severity: **Info** · Category: security (DoS)
- `src/gtk/remotectl.c:18`, `src/gtk/remotectl.c:735`, `src/gtk/remotectl.c:844`

```c
#define RC_MAX_CONNS        4
...
    c->auth_timer = g_timeout_add_seconds(RC_AUTH_TIMEOUT_S, rc_auth_expired, c);
```

Trigger: any local process opens four TCP connections to the ephemeral
`127.0.0.1` port (readable from `ss -ltnp`) and sends nothing or malformed
lines; each is held for 10 s, and `malformed request` replies do not close
the connection, so re-connecting keeps the legitimate MCP client locked out
indefinitely.

Consequence: local denial of service of the MCP/remote-control channel
only. Authentication itself is sound: 256-bit `RAND_bytes` token,
constant-time compare, `0600` token file in the state dir (which the
sandboxed renderer cannot read), loopback bind, bad token closes the
socket.

Fix: close the connection on the first malformed/unauthenticated line and
reserve one slot for already-authenticated clients; optionally require
`SO_PEERCRED`-style UID matching via a Unix socket instead of TCP.

---

### NET-14 — TLS "insecure override" page runs with the real origin's storage

- Severity: **Info** · Category: security
- `src/net.c:5874-5903`

```c
        if (opt_in && !hsts_pinned && req->top_level_navigation) {
            ...
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
            curl_easy_setopt(curl, CURLOPT_COOKIE, NULL);
            hctx->cookie_partition = NULL;
```

With `tls_allow_insecure_override` enabled (opt-in, prompts for sensitive
config changes), a failed certificate check is retried without
verification. Cookies are withheld, the cache is skipped and the UI shows
`NS_SEC_INVALID`, which is good; but the document is still scripted as
`https://host`, so a MITM page gets that origin's `localStorage`,
IndexedDB and session storage. Consider running the override as an opaque
origin or disabling storage APIs for `NS_SEC_INVALID` documents.

---

### What is done well

- The HTTP/IPC framing (`ipc_http.c`) is conservative: fixed-size header
  buffers with truncation, `NS_HTTP_MAX_HEADERS`, clamped numeric headers,
  `content_length` bounded by `NS_HTTP_MAX_BODY`/`NS_HTTP_MAX_REPLY`, and
  the shell re-validates frame geometry (`frame_head_fits`, `pl_texture`
  bounds-checks every shared-memory tile offset) before touching pixels.
- Shell-side consumers of renderer data are hardened: the download path
  must be a regular file directly inside the staging directory
  (`download_staging_path_ok`, no symlink following), suggested filenames
  are sanitised and executables prompt, capability requests are matched
  against the view's real origin, audio-helper commands are allow-listed
  and may only `open` files under `~/.cache/nordstjernen/msaudio`, and
  renderer-pushed config changes prompt for every non-cosmetic key.
- Networking policy is thorough: manual redirect handling with
  Authorization/Cookie stripping on origin change, https→http downgrade
  blocked for subresources, per-hop CSP and safe-browsing checks,
  `CURLOPT_PROTOCOLS_STR` limited to http/https/ftp, `X-ND-*` control
  headers rejected from page JS, `\r\n` filtered from headers and methods,
  and a private-network-access guard applied at socket-open time (immune
  to DNS rebinding within a request).
- The cookie store implements the modern rules correctly: `__Host-`/
  `__Secure-` prefixes, `SameSite=None` requires `Secure`, cookie-prefix
  forgery via nameless cookies rejected, public-suffix domain check,
  secure-cookie overlay protection, HttpOnly vs script, per-domain and
  per-partition caps, control-character rejection, and partitioning by
  top-level site.
- Decompression is bounded: streamed through zlib/brotli/zstd with an
  output cap derived from the remaining budget, at most five stacked
  codings, multi-member gzip handled, and `data:` URLs decoded against the
  same budget.
- The Linux sandbox is real: Landlock with `PR_SET_NO_NEW_PRIVS`, a
  default-`EPERM` seccomp filter that omits `execve`, `ptrace`, `mount`,
  `bpf`, `setuid`, `unshare`, denies `clone3`, masks namespace flags on
  `clone`, denies AF_UNIX sockets in the renderer, blocks `TIOCSTI`, and
  refuses to start without Landlock on non-Android Linux.

### Count by severity

| Severity | Count | IDs |
|----------|-------|-----|
| Critical | 0 | — |
| High     | 1 | NET-1 |
| Medium   | 3 | NET-2, NET-3, NET-4 |
| Low      | 8 | NET-5, NET-6, NET-7, NET-8, NET-9, NET-10, NET-11, NET-12 (unconfirmed) |
| Info     | 2 | NET-13, NET-14 |

---

## Nordstjernen HTML / DOM / image / font review

Static review of `src/html.c`, `src/html_lexbor.c`, `src/dom.c`, `src/dom.h`,
`src/xml.c`, `src/svg.c`, `src/mathml.c`, `src/image.c`, `src/image_ico.c`,
`src/image_wuffs.c`, `src/image_webp.c`, `src/image_jxl.c`, `src/image_avif.c`,
`src/font.c`, `src/woff2.c`, `src/selection.c`, `src/forms.c`, `src/datetime.c`,
plus the `srcset`/`<picture>` selection in `src/layout.c` and the form
submission / multipart code in `src/libnordstjernen.c`, `src/net.c`, `src/js.c`
that those files feed. HEAD `a58ff38`, no build available; every finding below
was confirmed by reading callers and allocation sites, and the charset
behaviour was additionally confirmed against glibc `iconv` on the review host.
Items I could not confirm are marked **unconfirmed**.

Spec references: WHATWG HTML Living Standard (`§` numbers), DOM Standard,
Encoding Standard, URL Standard.

---

### HTML-1 — Legacy-charset decode fails closed on a single undefined byte

- **Severity:** Medium  **Category:** bug
- **Where:** `src/html.c:765-779`, `src/html.c:807-825`

```c
char *out = g_convert(body, (gssize)len, "UTF-8", cs, NULL, NULL, NULL);
if (out) { charset_report(charset_out, cs); g_free(cs); return out; }
...
char *latin1 = g_convert(body, (gssize)len, "UTF-8", "WINDOWS-1252", NULL, NULL, NULL);
if (latin1) { ... return latin1; }
charset_report(charset_out, "UTF-8");
return g_utf8_make_valid(body, (gssize)len);
```

**Trigger.** A page declares (or uchardet detects) a legacy charset and the
body contains one byte that glibc's table leaves undefined. For
`windows-1252` those are 0x81, 0x8D, 0x8F, 0x90, 0x9D (verified:
`printf 'a\x81b' | iconv -f WINDOWS-1252 -t UTF-8` exits 1). `g_convert`
returns NULL for the whole body, the declared-charset path is abandoned, the
uchardet path usually re-detects the same charset and fails again, the
`WINDOWS-1252` last resort fails for the same byte, and the body ends up in
`g_utf8_make_valid`, which turns **every** non-ASCII byte in the document
into U+FFFD.

**Consequence.** One stray control byte (common in copy-pasted CMS content)
destroys all non-ASCII text on a legacy-encoded page. The Encoding Standard
defines decoding as "decode with replacement" (errors produce U+FFFD for the
offending byte only) and maps the five bytes above to U+0081 etc.

**Fix.** Decode in a loop: on `G_CONVERT_ERROR_ILLEGAL_SEQUENCE` use
`bytes_read` to copy the converted prefix, append U+FFFD (or, for
windows-1252, the identity mapping of 0x80-0x9F), skip one byte and continue.
Alternatively ship the Encoding Standard single-byte tables for the
windows-125x family and only fall back to iconv for multi-byte encodings.

---

### HTML-2 — Embedded NUL truncates the document and derails charset sniffing

- **Severity:** Medium  **Category:** bug / spec-compliance
- **Where:** `src/html.c:782-784`, `src/html.c:733` (API shape), `src/libnordstjernen.c:1912-1921`

```c
if (g_utf8_validate(body, (gssize)len, NULL)) {
    charset_report(charset_out, "UTF-8");
    return g_strndup(body, len);
}
...
/* libnordstjernen.c */
ns_node *doc = ns_html_parse_with_scripting(
    decoded ? decoded : "", decoded ? (gssize)strlen(decoded) : 0, scripting_on);
```

**Trigger.** Any HTML body containing a 0x00 byte. `g_utf8_validate` with a
positive length returns FALSE when any byte is NUL (documented GLib
behaviour), so an undeclared UTF-8 page is pushed through uchardet (which
reports UTF-8, which the code then ignores) and is decoded as windows-1252.
Independently, `ns_html_decode_body_full` hands back a `char *` without a
length and every caller re-measures it with `strlen`, so the parser never
sees anything after the first NUL.

**Consequence.** Mojibake of the whole page plus silent truncation of all
markup after the NUL. HTML §13.2.5 (tokenizer "data state") requires NUL to
be a parse error that yields U+FFFD, and §13.2.3 encoding sniffing is not
supposed to be affected by NUL bytes.

**Fix.** Return a length from the decoder (or replace NUL with U+FFFD /
drop it before validation), and pass that length to `ns_html_parse*`
instead of `strlen`.

---

### HTML-3 — Non-Encoding-Standard labels (`utf-16`, `utf-32`, EBCDIC, …) are honoured

- **Severity:** Medium  **Category:** spec-compliance (with robustness impact)
- **Where:** `src/html.c:387-422` (`charset_normalize`), `src/html.c:760-780`

```c
static char *charset_normalize(const char *name) { ... return g_ascii_strup(name, -1); }
...
char *cs = charset_normalize(declared);
if (g_ascii_strcasecmp(cs, "UTF-8") == 0) declared_utf8 = TRUE;
else if (!charset_is_dangerous(cs)) {
    char *out = g_convert(body, (gssize)len, "UTF-8", cs, NULL, NULL, NULL);
```

**Trigger.** `<meta charset="utf-16">` on an ordinary UTF-8/ASCII page (a
real-world authoring mistake the spec explicitly handles), or
`Content-Type: text/html; charset=utf-32` / `charset=IBM037`. Any label
iconv knows is accepted after upper-casing. Verified:
`printf '<html>' | iconv -f UTF-16 -t UTF-8` succeeds and yields three CJK
characters.

**Consequence.** The page renders as garbage. HTML §13.2.3.3 ("prescan …
if the encoding is a UTF-16 encoding, change it to UTF-8") and the Encoding
Standard ("get an encoding" returns failure for labels it does not define)
both require ignoring these labels. The file already contains the correct
WHATWG label table (`ns_encoding_labels`, used only by
`ns_html_declared_charset` for the iframe charset attribute) but the decode
path uses a different, smaller `charset_normalize` map plus a raw
pass-through to iconv.

**Fix.** Make `ns_html_decode_body_full` resolve the label with
`ns_encoding_label_to_name`, treat a NULL result as "no declaration", map
UTF-16 labels from `<meta>` to UTF-8, and only then hand the canonical name
(mapped to its iconv spelling) to `g_convert`.

---

### HTML-4 — Charset prescan is a substring grep, not the `<meta>` prescan

- **Severity:** Low  **Category:** spec-compliance
- **Where:** `src/html.c:424-443`, `src/html.c:762-763`

```c
for (gsize i = 0; i + 7 <= len; i++) {
    if (g_ascii_strncasecmp(s + i, "charset", 7) != 0) continue;
```

**Trigger.** Any occurrence of `charset=` in the first 1024 bytes that is
not a `<meta>` attribute, e.g. a comment, a `<title>`, `<a href="?charset=…">`,
or an inline `<script>` string. The first hit wins; a real `<meta charset>`
later in the head is ignored.

**Consequence.** Wrong encoding for the page. HTML §13.2.3.2 ("prescan a
byte stream") only inspects `<meta>` tags and its `charset`/`content`
attributes and skips comments and other tags.

**Fix.** Implement the prescan state machine (it is ~60 lines) or at least
require the match to be inside a `<meta …>` tag that precedes the first
non-head content.

---

### HTML-5 — Nested SVG opacity/mask/filter groups allocate unbounded surface memory

- **Severity:** Medium  **Category:** security (denial of service)
- **Where:** `src/svg.c:2803-2806`, `src/svg.c:1970-1979`, limits at `src/svg.c:20-42`

```c
cairo_surface_t *mask = svg_mask_surface(ctx, n, &st);
double blur = svg_filter_blur(ctx, n);
gboolean grouped = opacity < 1.0 || mask != NULL || blur > 0;
if (grouped) cairo_push_group(cr);
...
int w = (int)ceil(MAX(cx1, cx2) + ox);  int h = (int)ceil(MAX(cy1, cy2) + oy);
if (w <= 0 || h <= 0 || (double)w * h > (double)NS_SVG_MAX_PIXELS) return NULL;
cairo_surface_t *rgb = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
```

**Trigger.** An SVG image (`<img src>` or inline) sized at the allowed
maximum (4096×4096 → `NS_SVG_MAX_PIXELS`) containing 200 nested
`<g opacity=".99">` (or `mask="url(#…)"` chains up to `NS_SVG_MAX_DEPTH`=24).
`cairo_push_group` allocates a surface the size of the current clip extents
(64 MB here) for every level and all levels are live simultaneously until
the innermost element finishes; `svg_mask_surface` additionally allocates
`rgb` (64 MB) plus an A8 surface per level before recursing. Nesting is
only bounded by `NS_SVG_MAX_NESTING` (256) and `NS_SVG_MAX_DEPTH` (24), not by
bytes: 200 × 64 MB ≈ 12 GB of requests from a ~10 KB file.

**Consequence.** Renderer process OOM (or allocation failure cascading
into cairo error surfaces) from a single image; the per-image pixel cap and
the tile/image caches do not cover group surfaces.

**Fix.** Track cumulative group bytes in `svg_ctx` (clip-extent area × 4 at
each `cairo_push_group` / mask allocation) and stop grouping (render the
element ungrouped, or skip the mask) past a budget such as 256 MB; shrink
mask surfaces to the element's bounding box instead of the full clip
extents; lower `NS_SVG_MAX_NESTING` for *grouped* elements specifically.

---

### HTML-6 — Form reset destroys the default selectedness of `<select>` options

- **Severity:** Medium  **Category:** spec-compliance / bug
- **Where:** `src/dom.c:2587-2593`; selection state written at `src/js.c:38268-38270`, `src/js.c:50147-50149`

```c
} else if (strcmp(n->name, "select") == 0) {
    ns_element_remove_attr(n, "data-nd-noselect");
    for (ns_node *o = n->first_child; o; o = o->next_sibling) {
        if (ns_node_is_element_named(o, "option"))
            ns_element_remove_attr(o, "selected");
    }
}
```

**Trigger.** `<form><select><option>a</option><option selected>b</option></select>
<input type=reset></form>`; user picks `a`, then clicks reset (or script
calls `form.reset()`). The engine represents the user's choice by moving the
`selected` *content attribute* (js.c), so the reset path has no dirty flag
to clear and instead deletes every `selected` attribute. Options inside
`<optgroup>` are not even visited.

**Consequence.** After reset the select shows `a` (first option) instead of
the author default `b`, and `option.defaultSelected` is permanently false.
HTML §4.10.7 (the `select` reset algorithm: "set the selectedness of all
option elements to false, then run the selectedness setting algorithm",
which re-reads the `selected` content attributes) and §4.10.22.2.

**Fix.** Keep dirty selectedness in an internal attribute
(e.g. `data-nd-selected`) like inputs do with `data-nd-value`; reset removes
only the internal marker; walk `optgroup` children too.

---

### HTML-7 — Native form submission ignores `enctype` (no multipart, files dropped) and `method=dialog`

- **Severity:** Medium  **Category:** spec-compliance
- **Where:** `src/libnordstjernen.c:3993-4062`, `src/forms.c:135-138`

```c
gboolean is_post = method && g_ascii_strcasecmp(method, "post") == 0;
...
ns_form_collect_inputs(form, b->doc, b->doc, body, &first, clicked);
...
b->pending_post_ct = g_strdup("application/x-www-form-urlencoded");
/* forms.c */
if (type && (g_ascii_strcasecmp(type, "button") == 0 ||
             g_ascii_strcasecmp(type, "reset")  == 0 ||
             g_ascii_strcasecmp(type, "file")   == 0))
    goto recurse;
```

**Trigger.** Any `<form method=post enctype="multipart/form-data">` with an
`<input type=file>` submitted by the user without script; or
`<form method=dialog>` inside `<dialog>`.

**Consequence.** The body is always urlencoded, file controls are silently
omitted and `text/plain` is unsupported, so plain-HTML upload forms post an
empty-ish urlencoded body that servers reject. `method=dialog` is treated as
GET and navigates away instead of closing the dialog with `returnValue`
(no `"dialog"` handling exists in the native path; I could not confirm
whether `src/js.c` intercepts it earlier — **partially unconfirmed**).
HTML §4.10.21.3 steps "enctype"/"multipart/form-data"/"text/plain" and the
"dialog" method branch. `docs/HTML-compatibility.md` line 243 claims native
multipart submission is implemented in `src/libnordstjernen.c`; the code does
not contain it (the multipart serialiser only exists for `FormData` in
`src/js.c:1195`).

**Fix.** Build a real entry list (name, value or File) in `forms.c` and
reuse `ns_multipart_boundary` / `ns_multipart_quote_field` to serialise it
when `enctype` is multipart; handle `text/plain`; route `method=dialog` to
the dialog close path; correct the compatibility doc.

---

### HTML-8 — GET submission appends to an existing query and drops the fragment

- **Severity:** Low  **Category:** spec-compliance
- **Where:** `src/libnordstjernen.c:4049-4059`

```c
char *frag = strchr(abs_action, '#');
if (frag) *frag = '\0';
...
full = strchr(abs_action, '?')
    ? g_strdup_printf("%s&%s", abs_action, query->str)
    : g_strdup_printf("%s?%s", abs_action, query->str);
```

**Trigger.** `<form action="search?lang=en#top"><input name=q></form>`.

**Consequence.** Navigates to `search?lang=en&q=…` with no fragment;
HTML §4.10.21.3 "Mutate action URL" *sets* the query component (replacing
`lang=en`) and leaves the fragment in place.

**Fix.** Parse the action with `ns_url_*`, set the query, keep the fragment.

---

### HTML-9 — Entry-list serialisation skips newline normalisation and unnamed image buttons

- **Severity:** Low  **Category:** spec-compliance
- **Where:** `src/net.c:8099-8117` (`ns_form_urlencoded_append`), `src/dom.c:750-763`, `src/js.c:1256-1261`, `src/forms.c:108-109`, `src/forms.c:124-134`

```c
if (!name || !*name) goto recurse;      /* forms.c:109 — image buttons need x/y even unnamed */
...
for (const unsigned char *p = (const unsigned char *)s; *p; p++) {   /* net.c — no CRLF step */
```

**Trigger.** A `<textarea>` containing line breaks (the engine normalises
them to LF in `ns_textarea_normalized_value_dup`), or an unnamed
`<input type=image>` used as the submitter.

**Consequence.** `%0A` is sent where every other browser sends `%0D%0A`
(HTML §4.10.21.4 "convert to a list of name-value pairs", newline
normalisation); the unnamed image button contributes nothing instead of
the spec's `x`/`y` entries. The same missing CRLF step applies to non-file
values in the `FormData` multipart serialiser.

**Fix.** Add the CRLF normalisation step in one shared place before
percent-encoding / multipart writing; emit `x`/`y` when the image button has
no name (and the real click coordinates rather than `0`).

---

### HTML-10 — `maxlength`/`minlength` validation ignores the dirty-value flag

- **Severity:** Low  **Category:** spec-compliance
- **Where:** `src/forms.c:290-302`

```c
if (ns_form_control_length_limits_apply(n)) {
    const char *minlen = ns_element_get_attr(n, "minlength");
    const char *maxlen = ns_element_get_attr(n, "maxlength");
    glong vlen = (glong)g_utf8_strlen(value, -1);
```

**Trigger.** `<input maxlength=3 value="hello" required>` submitted without
the user touching the field.

**Consequence.** Submission is blocked. HTML §4.10.5.3.2 / §4.10.5.3.3:
`tooLong`/`tooShort` only apply when the value was last changed by a user
edit (dirty value flag true).

**Fix.** Gate the length checks on `data-nd-user-edited` (which the engine
already sets in `ns_node_set_editable_value`).

---

### HTML-11 — Whitespace text nodes between `<head>` and `<body>` are deleted

- **Severity:** Low  **Category:** spec-compliance
- **Where:** `src/html_lexbor.c:616-641`, called at `src/html_lexbor.c:724`

```c
while (c) {
    ns_node *next = c->next_sibling;
    if (c->kind == NS_NODE_TEXT && ns_text_all_whitespace(c->text)) {
        ns_node_remove(c);
        ns_node_free(c);
    }
```

**Trigger.** Any document with a newline between `</head>` and `<body>`
(i.e. almost all).

**Consequence.** `document.documentElement.childNodes.length` is 2 instead
of 3, `document.head.nextSibling` is `<body>` instead of a Text node, and
`outerHTML` round-tripping loses the whitespace. HTML §13.2.6.4.6 ("after
head" insertion mode: whitespace character tokens are inserted as children
of `html`). Lexbor builds the correct tree; this post-pass undoes it.

**Fix.** Drop the pass and let layout treat inter-element whitespace at
that level as collapsible (it already must for `<html>`'s other children).

---

### HTML-12 — Fragment parsing uses `<body>` as context for unknown/custom elements

- **Severity:** Low  **Category:** spec-compliance
- **Where:** `src/html_lexbor.c:737-746`, `src/html_lexbor.c:767-768`

```c
const lxb_tag_data_t *data = lxb_tag_data_by_name(hash, (const lxb_char_t *)name, strlen(name));
if (!data) return LXB_TAG_BODY;
...
char *lower = context_tag ? g_ascii_strdown(context_tag, -1) : NULL;
```

**Trigger.** `customEl.innerHTML = …` or any element whose tag is not in
lexbor's static table; also camel-cased SVG context names are lower-cased
before lookup (whether lexbor's table resolves `foreignobject` is
**unconfirmed**).

**Consequence.** HTML §13.4 ("parsing HTML fragments") makes the context
element itself the root of the stack of open elements; using `body`
changes behaviour for the "in body" vs. special-element rules (e.g. nested
`<p>`/`<form>` handling) and for `template`/table contexts when the name
lookup fails.

**Fix.** Register unknown names with `lxb_tag_data_by_name` on the
document's tag hash (lexbor supports dynamic tags) instead of substituting
`body`; preserve case for SVG/MathML contexts.

---

### HTML-13 — `setAttribute` on SVG/foreign elements matches case-insensitively

- **Severity:** Low  **Category:** spec-compliance
- **Where:** `src/dom.c:1399-1409` vs. lookup at `src/dom.c:1632-1636`

```c
for (ns_attr *a = el->attrs; a; a = a->next) {
    if (g_ascii_strcasecmp(a->name, name) == 0) {      /* set: case-insensitive */
...
if (el->flags & NS_NODE_SVG_NS) {
    for (const ns_attr *a = el->attrs; a; a = a->next)
        if (a->name && strcmp(a->name, name) == 0)      /* get: exact */
```

**Trigger.** `svg.setAttribute("viewbox", "0 0 1 1")` on an element that
already has `viewBox`.

**Consequence.** The existing `viewBox` value is overwritten and
`getAttribute("viewbox")` still returns null. DOM §4.9 `setAttribute` only
lower-cases for HTML-namespace elements in HTML documents and otherwise
matches the qualified name exactly.

**Fix.** In `ns_element_set_attr_len` / `ns_element_remove_attr`, use
`strcmp` when `el->flags & (NS_NODE_SVG_NS|NS_NODE_FOREIGN_NS|NS_NODE_KEEP_CASE)`.

---

### HTML-14 — Prefixed SVG elements from the XML parser are recognised but never drawn

- **Severity:** Low  **Category:** bug / spec-compliance
- **Where:** `src/xml.c:316-318`, `src/svg.c:2504-2505`, `src/svg.c:1076-1167`

```c
char *stored = (is_xhtml && colon) ? g_strdup(colon + 1) : g_strdup(qname);   /* xml.c */
...
const char *colon = strchr(tag, ':'); if (colon) tag = colon + 1;             /* svg_renderable */
...
if (strcmp(tag, "path") == 0) {                                               /* svg_shape_path: full name */
```

**Trigger.** An XHTML/XML document using `<svg:svg xmlns:svg="…"><svg:rect …/>`.

**Consequence.** `svg_renderable` strips the prefix and accepts the
element, but `svg_shape_path`, `svg_render_node_nested` and `svg_by_id`
users compare the full `svg:rect`, so nothing is painted. Per XML
Namespaces / DOM, `localName` should be `rect` and the prefix kept
separately (the XHTML branch already does this).

**Fix.** Store the local name for every namespaced element (not only
XHTML) and keep the prefix in `data-nd-ns-prefix` as the XHTML branch does.

---

### HTML-15 — Unbounded `double` → `long` casts when serialising date/time inputs

- **Severity:** Low  **Category:** bug
- **Where:** `src/dom.c:373-374`, `src/dom.c:380`, `src/dom.c:389`, `src/dom.c:406-407`

```c
double days = floor(v / 86400000.0 + 0.5);
ns_dt_civil_from_days((long)days, &y, &m, &d);
```

**Trigger.** `input.type="date"; input.step="1e300"; input.stepUp()` or a
`valueAsNumber` setter with a huge magnitude reaching
`ns_input_number_to_value`.

**Consequence.** Out-of-range floating→integer conversion is undefined
behaviour in C (on x86-64 it yields `LONG_MIN`); the subsequent year check
rejects the result so no memory issue was found, but UBSan builds will
abort and other targets may behave differently.

**Fix.** Range-check `v` against the representable day/month range before
casting (the year bound `NS_DT_MAX_YEAR` = 275760 gives the limit).

---

### HTML-16 — `<img>` source selection honours non-standard `data-src*` attributes over `src`/`srcset`

- **Severity:** Low  **Category:** spec-compliance
- **Where:** `src/layout.c:5339-5365`, `src/layout.c:5304-5305`

```c
const char *dsrc   = ns_element_get_attr(n, "data-src");
if (!dsrc || !*dsrc) dsrc = ns_element_get_attr(n, "data-original");
if (!dsrc || !*dsrc) dsrc = ns_element_get_attr(n, "data-lazy-src");
const char *dsset  = ns_element_get_attr(n, "data-srcset");
...
char *u = srcset_select(dsset, sizes, NULL, density);
if (u && (srcset_has_width_descriptor(dsset) || !dsrc || !*dsrc)) return u;
```

**Trigger.** Any page that stores unrelated URLs in `data-src`
(zoom/high-res variants, template placeholders, A/B candidates) or whose
lazy-loader expects to swap `data-src` → `src` itself.

**Consequence.** The browser fetches and displays a different image than
`src`/`srcset` select per HTML §4.8.4.3.4 ("update the image data" uses
only `src`, `srcset`, `sizes` and `<source>`), may double-load when the
site's script later swaps the attribute, and `currentSrc` reports a URL the
spec never selects. This is a generic heuristic, not a host hack, but it is
still an engine deviation that real sites trip over.

**Fix.** Remove the `data-*` fallbacks (the engine executes the site's
lazy-load JS and supports `loading=lazy`), or restrict them to the case
where `src` is absent/empty.

---

### HTML-17 — Web fonts are installed process-wide under the page-chosen family name

- **Severity:** Low (**scope unconfirmed**)  **Category:** security
- **Where:** `src/font.c:730-742`, `src/font.c:809-819`, `src/font.c:49` (`g_entries` global)

```c
FcPatternDel(pat, FC_FAMILY);
FcPatternDel(pat, FC_FAMILYLANG);
FcPatternAddString(pat, FC_FAMILY, (const FcChar8 *)family);
...
FcConfigAppFontAddFile(NULL, (const FcChar8 *)path);
```

**Trigger.** `@font-face { font-family: "DejaVu Sans"; src: url(x.woff2) }`
(any system family name).

**Consequence.** The face is added to fontconfig's application set with a
higher `FC_FONTVERSION` than the system face and no document/origin scoping
in `g_entries`, so for the remaining lifetime of the renderer process other
documents rendered by that process (and any later page relying on the
system font by name) resolve the attacker's glyphs — a glyph-remapping font
can alter displayed text on unrelated pages. Whether the renderer process
is per-site/per-tab (which would contain the effect) is outside the
reviewed files, hence unconfirmed.

**Fix.** Scope installed faces to the owning document (private family
alias, as already done for emoji faces via `ns_font_private_family`) and
remove them from the application set when the document is destroyed.

---

### HTML-18 — SVG arc computation can feed non-finite coordinates to cairo

- **Severity:** Low (**unconfirmed crash**)  **Category:** bug / hardening
- **Where:** `src/svg.c:849-911`

```c
double lam = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry);
if (lam > 1.0) { double k = sqrt(lam); rx *= k; ry *= k; }
double den = rx * rx * y1p * y1p + ry * ry * x1p * x1p;
```

**Trigger.** `<path d="M0 0 A 1e-200 1 0 0 1 100 100"/>`: `svg_num` only
rejects non-finite *literals*; `lam` overflows to `inf`, `rx` becomes
`inf`, `den`/`num` become NaN and `cxp`/`cyp` are NaN, which reach
`cairo_curve_to`.

**Consequence.** Cairo converts NaN/inf through `_cairo_fixed_from_double`
without checking; in practice this yields garbage geometry or an error
surface, not memory corruption, but it has historically triggered
assertion failures in the stroker. Not confirmed on this host.

**Fix.** After computing the centre and angles, bail out
(`cairo_line_to(x2, y2)`) if any of `cx, cy, t1, t2, rx, ry` is non-finite.

---

### HTML-19 — Script source positions come from a textual `<script` scan

- **Severity:** Info  **Category:** bug
- **Where:** `src/html_lexbor.c:655-695`

```c
if (input[i] == '<' && i + 7 <= len &&
    g_ascii_strncasecmp(input + i, "<script", 7) == 0 && ...
```

A `<script` inside a comment, a string literal, or a `<textarea>` shifts
the line/column assigned to every following real script, so JS error
messages point at the wrong place. Lexbor does not expose token offsets,
so this is a known approximation; mentioned for completeness.

---

### HTML-20 — Standalone SVG images are parsed with the HTML parser

- **Severity:** Info  **Category:** spec-compliance
- **Where:** `src/svg.c:3843`, `src/svg.c:3891`, sniff at `src/svg.c:3426-3437`

`image/svg+xml` resources are XML and should go through `ns_xml_parse`
(well-formedness errors must not render). The HTML parser's foreign-content
rules make most SVG files work, but camel-cased *unknown* element names are
lower-cased, DTD entities are unsupported, and the loose "`<svg` anywhere in
the first 1 KB" sniff lets an HTML document served to `<img>` render its
embedded SVG. Pragmatic, documented as 🟡 in `docs/HTML-compatibility.md`.

---

### HTML-21 — Obsolete `shadowroot` attribute still enables declarative shadow DOM

- **Severity:** Info  **Category:** spec-compliance
- **Where:** `src/html_lexbor.c:231-232`

HTML now only recognises `shadowrootmode`; the pre-standard `shadowroot`
attribute was removed in 2023 and other engines no longer honour it.

---

### HTML-22 — `pattern` validation runs through PCRE2, not ECMAScript `v`-mode

- **Severity:** Info  **Category:** spec-compliance
- **Where:** `src/forms.c:183-196`

`^(?:…)$` under GRegex differs from the required `v`-flag ECMAScript
semantics (set operations `[\p{L}--[a-z]]`, `\p{…}` property escapes
without the `u` flag, `$` matching before a trailing newline). The length
caps (2048 / 10000) keep this from being a ReDoS vector; a compile error
currently *passes* validation, which matches the spec's "if the pattern is
not a valid regex, the attribute is ignored".

---

### What is done well

- Every raster format is decoded by memory-safe Wuffs (or a hardened
  library) behind uniform caps: 16384 px per side, 64 M pixels, bounded
  frame counts and a 512 MB total for animations, 64 MB input caps — and the
  ICO container parser cross-checks `hdr`, palette, XOR and AND-mask offsets
  against the entry length before every `memcpy`.
- `src/woff2.c` is a model of defensive binary parsing: all reads go through
  a bounds-checked `w2_reader`, sub-streams are sliced with length checks,
  glyph point counts are capped at 0xFFFF, reconstructed output is capped
  at 64 MB, Brotli output length must match the directory total exactly.
- DOM freeing and index walks are iterative (`ns_node_free`,
  `ns_doc_index_walk`), and every recursive walker in `dom.c`, `forms.c`,
  `html_lexbor.c`, `xml.c`, `svg.c`, `mathml.c` carries a depth guard
  (`NS_DOM_MAX_DEPTH`, `XML_MAX_DEPTH`, `NS_SVG_MAX_DEPTH/NESTING/NODES`,
  `NS_MATH_MAX_DEPTH`), so hostile nesting cannot exhaust the stack.
- The borrowed-string design (DOM strings point into the lexbor document)
  is consistently closed: every fragment-move site in `src/js.c` calls
  `ns_node_own_strings_deep` before the backing document is destroyed, and
  `ns_node_own_strings_one` clears the class-token cache that aliases the
  attribute value.
- The multipart serialiser uses a CSPRNG boundary, applies the spec's exact
  field-name/filename escaping (`"`→`%22`, CR→`%0D`, LF→`%0A`) and strips
  control bytes from `Content-Type`; the urlencoded serialiser uses the
  exact `application/x-www-form-urlencoded` safe set.
- Known-dangerous charsets (UTF-7, HZ-GB-2312, ISO-2022-CN/KR, CESU/BOCU/SCSU)
  are explicitly refused before iconv sees them; `srcset` descriptor parsing
  follows §4.8.4.3.11 faithfully, including the parenthesis state and the
  "future-compat h" rules.

### Count by severity

| Severity | Count | IDs |
|----------|-------|-----|
| Critical | 0 | — |
| High | 0 | — |
| Medium | 6 | HTML-1, HTML-2, HTML-3, HTML-5, HTML-6, HTML-7 |
| Low | 12 | HTML-4, HTML-8, HTML-9, HTML-10, HTML-11, HTML-12, HTML-13, HTML-14, HTML-15, HTML-16, HTML-17, HTML-18 |
| Info | 4 | HTML-19, HTML-20, HTML-21, HTML-22 |
| **Total** | **22** | |

---

## Nordstjernen CSS engine review (static, HEAD a58ff38)

Scope: `src/css.c` (read in full, 39k lines), `src/css.h`, `src/css_syntax.c`,
`src/css_grammar.c`, `src/css_color.c`, `src/css_media.c`,
`src/css_prop_syntax.c`, `src/css_shape.c`, `src/css_anchor.c`,
`src/css_filter.c`, `src/css_sizes.c`, plus the callers in `src/engine.c` and
`src/js.c` needed to confirm findings. No build was possible; every finding
below was confirmed by reading the code path end to end (allocation sizes,
callers, guards). Items I could not fully confirm are marked *unconfirmed*.

Severity key: Critical / High / Medium / Low / Info.
Category key: security | bug | spec-compliance.

---

### CSS-1 — `:visited` is styled with no history-leak mitigation (process-global visited set)

- **Severity:** Medium  **Category:** security (privacy)
- **Where:** `src/css.c:25148-25215` (`g_visited_urls`, `ns_el_is_visited_link`),
  `src/css.c:26197-26201` (`NS_CSS_PC_VISITED` in `match_simple`),
  `src/js.c:70284` (the only producer, `ns_js_mark_iframe_source`).

```c
static GHashTable *g_visited_urls = NULL;
...
static gboolean
ns_el_is_visited_link(const ns_node *el)
{
    if (!g_visited_urls || !g_css_doc_base || !ns_el_is_link(el)) return FALSE;
    const char *href = ns_element_get_attr(el, "href");
    char *abs_url = ns_url_resolve(g_css_doc_base, href);
    gboolean v = g_hash_table_contains(g_visited_urls, abs_url);
```

- **Trigger:** `g_visited_urls` is a single process-wide set, filled whenever
  any document loads an `<iframe src=URL>` (`js.c:70284`). Any later page in
  any origin can write `a:visited { color: red }` (or `background-image:
  url(https://attacker/…)`, `width`, etc.) and read the result back via
  `getComputedStyle` / `offsetWidth` — nothing in `js.c`, `paint.c` or
  `layout.c` special-cases `:visited` (grep for "visited" finds only the
  producer). Selectors 4 §9.2 explicitly permits UAs to lie about `:visited`
  for privacy; every major engine restricts it to colour-only, hides it from
  computed style, and partitions it per top-level site.
- **Consequence:** Cross-site disclosure of iframe URLs embedded by other
  pages in the same session (e.g. OAuth/account frames). Today the set is only
  fed by iframe loads; if `ns_css_mark_visited` is ever wired to real
  navigation history (it is exported in `css.h` for that purpose) this becomes
  classic history sniffing.
- **Fix:** Either treat all links as unvisited (`ns_el_is_visited_link` → FALSE)
  or implement the standard mitigation: allow `:visited` to change only
  `color`/`background-color`/`border-*-color`/`outline-color`/`fill`/`stroke`
  (and never to introduce `url()`), make `getComputedStyle` return the
  `:link` value, and key `g_visited_urls` by top-level site.

### CSS-2 — `:has()` evaluation has no operation budget: O(N·depth·G) subtree walks

- **Severity:** Medium  **Category:** security (DoS) / bug
- **Where:** `src/css.c:25797-25842` (`relative_descendant_matches`,
  `relative_chain_matches`), memo at `src/css.c:25878-25895`, budget only in
  `src/css.c:28343-28392` (`match_complex_chain`).

```c
static gboolean
relative_descendant_matches(const ns_css_selector *rel, const ns_node *anchor,
                            const ns_node *base, guint idx, int depth)
{
    if (depth >= 512) return FALSE;
    for (const ns_node *c = base ? base->first_child : NULL; c; c = c->next_sibling) {
        if (c->kind != NS_NODE_ELEMENT) continue;
        if (relative_try_candidate(rel, anchor, c, idx, depth)) return TRUE;
        if (relative_descendant_matches(rel, anchor, c, idx, depth + 1))
            return TRUE;
```

- **Trigger:** `NS_SEL_MATCH_BUDGET` (8M ops) is charged only in the
  ancestor/sibling chain walker; the `:has()` descendant walk charges nothing.
  A page with a few hundred 500-deep chains (≈10^5 nodes, allowed by
  `NS_DOM_MAX_DEPTH 512`) and a stylesheet with ~1000 distinct universal
  `:has(.cN)` rules makes every element's subtree be walked once per group
  (`g_has_memo` keys on `(group, anchor)`, so it de-duplicates repeats but not
  the N·depth·G product): ≈ 200·512²/2 · 1000 ≈ 2.6·10^10 node visits per
  restyle. The incremental invalidation in `incr_mark_loose_has_subjects`
  re-dirties the subjects on every mutation, so a timer flipping one class
  keeps the renderer pinned.
- **Consequence:** Renderer hang from a crafted page (the docs claim
  "✅ (bounded)" for `:has()`).
- **Fix:** Charge `g_sel_match_ops` inside `relative_descendant_matches` /
  `relative_try_candidate` and bail with a negative result when the budget is
  exhausted; alternatively cap the total `:has()` subtree visits per restyle.

### CSS-3 — Linked stylesheets are cached by URL only; changed content is never re-parsed

- **Severity:** Medium  **Category:** bug
- **Where:** `src/css.c:38510-38536` (`ns_css_stylesheet_parse_url_cached`),
  caller `src/engine.c:1583`.

```c
    char *key = g_strdup_printf("%.0fx%.0f|%s",
                                ns_css_media_viewport_current_w(),
                                ns_css_media_viewport_current_h(), url);
    sheet_cache_note_used(key);
    ns_css_stylesheet *hit = g_hash_table_lookup(g_link_sheet_cache, key);
    if (hit) {
        g_free(key);
        return hit;
    }
```

- **Trigger:** `engine.c` fetches the bytes (honouring HTTP caching/SRI) and
  passes them in, but the parsed sheet is looked up by viewport+URL alone and
  the passed `css`/`len` are ignored on a hit. The sibling
  `ns_css_stylesheet_parse_import_cached` (`css.c:38549`) does compare bytes
  (`g_bytes_equal`). Only `ns_css_stylesheet_cache_drop` (media-state
  changes) or LRU eviction after >256 entries and 512 passes clears the entry.
- **Consequence:** Reloading a page whose stylesheet changed at the same URL
  (CMS edits, dev servers, `blob:` reuse, A/B-served CSS) keeps rendering the
  stale rules for the rest of the session; also a stale sheet can keep
  resolving `url()`s against the old content.
- **Fix:** Include a content digest (or the `GBytes` pointer/`g_bytes_equal`
  check as the import path does) in the key, or compare `len`+hash on hit.

### CSS-4 — `:not()` with an empty (or depth-exhausted) argument matches everything

- **Severity:** Low  **Category:** spec-compliance
- **Where:** `src/css.c:2900-2906`, `src/css.c:2075-2097`.

```c
                } else if (name_n == 3 && arg_s &&
                           g_ascii_strncasecmp(name_s, "not", 3) == 0) {
                    GPtrArray *group = parse_selector_group(arg_s, arg_n, depth + 1);
                    if (group->len == 0) {
                        g_ptr_array_free(group, TRUE);
                    } else {
```

- **Trigger:** `div:not()` or `div:not(  )`, and any `:not(...)` nested deeper
  than `NS_CSS_MAX_SELECTOR_NESTING` (48), yields an empty group which is
  silently dropped, so the compound degrades to `div` and matches every
  `div`. `g_sel_parse_error` is not set (only in strict `@supports` mode).
- **Spec:** Selectors 4 §4.4: `:not()` takes a `<complex-real-selector-list>`
  which is *not* forgiving; an empty list is invalid and the whole rule must
  be dropped. (Compare `:is()`/`:has()` here, which correctly become
  `never_match`.)
- **Fix:** When `group->len == 0` set `g_sel_parse_error = TRUE` (and
  `cmp->never_match = TRUE` for the depth-exhausted case).

### CSS-5 — `calc()` accepts `+`/`-` without surrounding whitespace

- **Severity:** Low  **Category:** spec-compliance
- **Where:** `src/css.c:3687-3700` (`calc_expr_parse`); compare the
  correct check in `math_sum_parse` `src/css.c:4232` and `css_color.c:1051`.

```c
        char op = *p++;
        ns_calc_term rhs;
        if (!calc_product_parse(&p, end, &rhs, depth)) return FALSE;
```

- **Trigger:** `width: calc(100px -20px)` or `calc(1px+2px)` parses as a
  subtraction/addition.
- **Spec:** Values 4 §10.1: "whitespace is required on both sides of the `+`
  and `-` operators"; `100px -20px` tokenises as two dimensions and the
  declaration is invalid. The serializer path (`math_sum_parse`) already
  enforces this, so specified and computed behaviour disagree.
- **Fix:** Require `is_ws(p[-1])` before and `is_ws(*p)` after the sign in
  `calc_expr_parse`, as `math_sum_parse` does.

### CSS-6 — `round()` ties use half-away-from-zero instead of half-up

- **Severity:** Low  **Category:** spec-compliance
- **Where:** `src/css.c:3899-3914` (`css_round_step`); `css_color.c:1016`
  (`round_to`) is correct, so colour channels and lengths disagree.

```c
    double q = a / fabs(b);
    double rq = strategy == 1 ? ceil(q) :
                strategy == 2 ? floor(q) :
                strategy == 3 ? trunc(q) : round(q);
```

- **Trigger:** `round(-2.5px, 1px)` gives `-3px`; `round(-25px, 10px)` gives
  `-30px`.
- **Spec:** Values 4 §10.4 `round()`, `nearest`: "If the two values are
  equally close, choose the one in the positive direction (the larger
  value)" → `-2px` / `-20px`.
- **Fix:** Use `floor(q + 0.5)` (as `css_color.c` does).

### CSS-7 — `min()`/`max()` with more than 8 arguments silently discard arguments

- **Severity:** Low  **Category:** bug / spec-compliance
- **Where:** `src/css.c:4582-4603`.

```c
            if (q == body_end || (*q == ',' && depth == 0)) {
                int slot = n < 8 ? n : 7;
                char *part = g_strndup(seg, (gsize)(q - seg));
```

- **Trigger:** `width: min(9px, 8px, 7px, 6px, 5px, 4px, 3px, 2px, 1px)`:
  arguments 8 and 9 overwrite slot 7, then `n` is clamped to 8; the result
  uses only the first seven values plus the last one. Memory-safe (slot
  clamped) but wrong. Values 4 §10.2 allows any number of arguments.
- **Fix:** Fold the comparison online (track running min/max and whether any
  arg is basis-dependent) instead of storing a fixed 8-slot array.

### CSS-8 — Dimension parser accepts C99 hex-floats and overflowing exponents

- **Severity:** Low  **Category:** spec-compliance
- **Where:** `src/css.c:3101-3112` (`parse_length`).

```c
    const char *num_start = p;
    while (*p && (g_ascii_isdigit(*p) || *p == '.')) p++;
    if (p == num_start) return FALSE;
    char *end = NULL;
    double v = g_ascii_strtod(text, &end);
```

- **Trigger:** The pre-scan only requires a leading digit; `g_ascii_strtod`
  then behaves like C-locale `strtod`, so `width: 0x10px` parses as `16px`
  and `1e999px` as `+inf` (later clamped by `calc_value_clamp_finite`, but
  the declaration is accepted rather than invalid). The real tokenizer
  (`css_syntax.c:103-129`) rejects both, so `@supports`/registered-property
  validation disagree with the property parser.
- **Spec:** Syntax 3 §4.3.12 number grammar admits only decimal digits,
  `.`, and an `e±` exponent.
- **Fix:** Scan the number with the Syntax-3 grammar (`css_syntax_number_end`)
  and only hand that span to `g_ascii_strtod`.

### CSS-9 — `url()` rewriting in raw declaration text ignores string tokens and quoted `)`

- **Severity:** Low  **Category:** bug
- **Where:** `src/css.c:24213-24254` (`css_raw_text_resolve_urls`), used for
  custom-property values and `var()`-pending declarations.

```c
        const char *hit = strstr(p, "url(");
        if (!hit) { g_string_append(out, p); break; }
        const char *close = strchr(hit + 4, ')');
```

- **Trigger:** `--label: "see url(x)"; content: var(--label)` rewrites the
  string's contents to an absolute URL; `--bg: url("a)b.png")` truncates at
  the first `)`; `/* url(x) */` inside a value is rewritten too.
- **Spec:** Syntax 3 §4.3.6: `url(` inside a string is just string content;
  `url("…")` with a quoted argument is a function token whose string may
  contain `)`.
- **Fix:** Walk the text with `css_scan_until`/`css_find_function` (which
  already skip strings and comments) instead of `strstr`/`strchr`.

### CSS-10 — A NUL byte truncates the rest of the stylesheet

- **Severity:** Low  **Category:** spec-compliance / bug
- **Where:** `src/css.c:24186-24188` (`css_stylesheet_parse`),
  `src/css.c:20512-20516` (`css_scan_until`, `stop[0] = TRUE`).

```c
    char *flattened = css_flatten_nesting(text, len_in);
    const char *p   = flattened;
    const char *end = flattened + strlen(flattened);
```

- **Trigger:** `css_flatten_nesting` copies the full `len_in` bytes (NULs
  included) into a `GString`, but the parser then measures it with `strlen`,
  and `css_scan_until` treats `\0` as a hard terminator. A stylesheet whose
  decoded text contains U+0000 (e.g. a mis-labelled UTF-16 file, or a
  deliberate NUL to hide rules from this engine only) loses everything after
  it.
- **Spec:** Syntax 3 §3.3 preprocessing: U+0000 is replaced by U+FFFD.
- **Fix:** Replace NUL with U+FFFD in `css_flatten_nesting` (or before it) and
  use `flattened_len` instead of `strlen`.

### CSS-11 — Component-value tokenizer rejects `\`+newline inside strings

- **Severity:** Low  **Category:** spec-compliance
- **Where:** `src/css_syntax.c:189-200` (`css_syntax_consume_string`) with
  `css_syntax_escape_end` at `:46-52`; the other string reader
  `src/css.c:966-985` handles it correctly.

```c
        if (c == '\\' && parser->offset < parser->length) {
            gsize escape = parser->offset - 1;
            gsize next = css_syntax_escape_end(parser->input, parser->length, escape);
            if (next == escape) {
                parser->valid = FALSE;
                break;
```

- **Trigger:** `--s: "a\<newline>b"` or a `filter`/`clip-path`/registered
  property value containing a string with an escaped line break is marked
  invalid.
- **Spec:** Syntax 3 §4.3.5 "consume a string token": for `\` followed by a
  newline, consume the newline and continue (line continuation).
- **Fix:** Treat `\` + newline as a continuation (skip both) instead of
  returning `offset - 1`.

### CSS-12 — `An+B` parser strips all whitespace before validating

- **Severity:** Low  **Category:** spec-compliance
- **Where:** `src/css.c:2161-2170` (`parse_anb`).

```c
    char *trimmed = g_strstrip(raw);
    char *s = g_malloc(strlen(trimmed) + 1);
    char *w = s;
    for (const char *r = trimmed; *r; r++)
        if (!is_ws(*r)) *w++ = *r;
```

- **Trigger:** `:nth-child(2 n)`, `:nth-child(1 2)` and `:nth-child(- n + 3)`
  are accepted (as `2n`, `12`, `-n+3`).
- **Spec:** Syntax 3 §9 `<an+b>`: whitespace is allowed only around the sign
  between the `n` term and `B`; `2 n` tokenises as number + ident and is
  invalid. Also note `anb_int_strict` clamps `A`/`B` to ±1,000,000
  (`ns_parse_int`), silently changing large coefficients.
- **Fix:** Tokenise per the spec grammar instead of deleting whitespace.

### CSS-13 — `!important` in the `style` attribute always beats `!important` in cascade layers (verify)

- **Severity:** Low  **Category:** spec-compliance
- **Where:** `src/css.c:30147-30156` (`match_cmp`), same pattern in
  `var_match_cmp`/`pending_match_cmp`.

```c
    if (a->important != b->important) return a->important ? 1 : -1;
    if (a->origin    != b->origin) ...
    if (a->inline_style != b->inline_style) return a->inline_style ? 1 : -1;
    int layer_cmp = css_layer_cmp(a->layer_order, b->layer_order, a->important);
```

- **Trigger:** `@layer a { div { color: red !important } }` vs
  `<div style="color: blue !important">`: the inline declaration wins here.
- **Spec:** Cascade 5 §6.4: the style attribute is treated as *unlayered*;
  for normal declarations unlayered wins over layered, but the importance
  inversion applies, so important *layered* declarations win over important
  unlayered/style-attribute ones (this is what Blink/Gecko ship). The code
  implements the inversion for rules (`css_layer_cmp`) but short-circuits
  `inline_style` before it. Marked *verify*: confirm against WPT
  `css-cascade/layer-important*.html` since I could not run the browser.
- **Fix:** Fold `inline_style` into the layer comparison (treat it as the
  highest unlayered position so it inverts with importance).

### CSS-14 — `attr()` taint check misses `image()` and other URL-bearing functions (unconfirmed)

- **Severity:** Low  **Category:** security
- **Where:** `src/css.c:31767-31770` (`substitute_attrs`), `:31711-31722`
  (`url_function_at`).

```c
            if (tainted && (strstr(val, "url(") || strstr(val, "src(") ||
                            strstr(val, "image-set(")))
                *tainted = TRUE;
```

- **Trigger:** `background-image: attr(data-x type(*))` with
  `data-x="image('https://attacker/leak')"`: `attr()` is not inside a URL
  function, and the substituted value contains `image(` (which
  `url_function_at` itself lists as URL-bearing) but is not flagged tainted,
  so the declaration is kept. *Unconfirmed* impact: I did not verify that
  `image()` reaches the fetch pipeline (only `image-set()` canonicalisation
  mentions it at `css.c:7680`).
- **Spec:** Values 5 §9.3 (`attr()` taint): any `url()`-like value produced
  from `attr()` must make the declaration invalid at computed-value time.
- **Fix:** Reuse the same function list as `url_function_at` (and ideally
  re-scan the substituted value with `css_find_function`) when deciding
  `tainted`.

### CSS-15 — `device-width`/`device-height` changes do not invalidate parse-time media evaluation

- **Severity:** Low  **Category:** bug
- **Where:** `src/css_media.c:23-28` (`ns_css_set_device_size`) vs. the
  cache drops in `:31-36`, `:1074-1079`, `src/css.c:21091-21108`.

```c
void
ns_css_set_device_size(double w, double h)
{
    if (w > 0) g_mq_device_w = w;
    if (h > 0) g_mq_device_h = h;
}
```

- **Trigger:** `@media` blocks are decided while parsing
  (`parse_rules_until`, `css.c:23560`), and the parsed sheets are cached
  with keys that include the viewport but not the device size. Moving the
  window to a monitor of a different size (or a DPI change that updates the
  device size without changing `dppx`) keeps `(device-width …)` results
  stale until something else drops the cache.
- **Fix:** Call `ns_css_stylesheet_cache_drop()` when the device size
  actually changes, like the other setters do.

### CSS-16 — Empty attribute selector `[]` is silently dropped from the compound

- **Severity:** Info  **Category:** spec-compliance
- **Where:** `src/css.c:2944-2951`.

```c
                if (!attr_name || !*attr_name) {
                    g_free(attr_name);
                    char term = 0;
                    const char *close = css_scan_until(p, end, "]", &term);
                    p = term == ']' ? close + 1 : close;
                    continue;
                }
```

- **Trigger:** `div[] { … }` and `div[ = x]` match every `div`; Selectors 4
  §6 requires an attribute name, so the rule should be invalid.
- **Fix:** Set `g_sel_parse_error = TRUE` in that branch.

### CSS-17 — `(int)(v + 0.5)` on a clamped `column-count` is undefined behaviour

- **Severity:** Info  **Category:** bug
- **Where:** `src/css.c:1281-1284` (`ns_css_used_column_count`); value
  clamped to `[INT_MIN, INT_MAX]` by `css_integer_clamp` at `:11530`.

```c
        double v = s->values[NS_CSS_COLUMN_COUNT]->u.length.v;
        if (v >= 2) n = (int)(v + 0.5);
```

- **Trigger:** `column-count: 2147483647` → `v + 0.5` is not representable
  as `int`; on x86-64 this yields `INT_MIN`, which `layout.c:17569`
  (`if (n_cols > 1)`) then treats as a single column, so it is harmless in
  practice but UB (and a sanitizer build will flag it).
- **Fix:** `n = (int)CLAMP(v + 0.5, 2, 1000)` or similar.

---

### Spec spot-checks that passed (docs/CSS-compatibility.md claims)

- Cascade 5: origin/importance ordering, `revert`, `revert-layer` and the
  non-standard `revert-rule` roll back correctly through
  `cascade_rollback_value` (`css.c:32901`); layer trees are ranked per
  §6.4 with sublayer/implicit-final-sublayer handling
  (`css_layer_ranks_finalize`, `css.c:30120`); `all` excludes
  `direction`/`unicode-bidi` (`css.c:18901`).
- Selectors 4: `:is()`/`:where()` forgiving, `:where()` zero specificity,
  `:has()` not nestable and specificity = max of arguments
  (`css.c:2846-2898`), `:nth-child(… of S)` specificity (`css.c:3002`),
  `:empty` ignores comments, pseudo-element compound ordering rules.
- Custom Properties: `var()` fallback, guaranteed-invalid → `unset`,
  registered `initial-value`/`inherits`/`syntax` validation (`@property`
  rule validity at `css.c:23700-23712` matches Properties & Values 1 §3),
  CSS-wide keywords inside custom properties.
- Color 4/5: gamut/space conversions, `none` carry-forward, powerless hue,
  legacy vs modern serialization, `color-mix()` weight normalisation and
  alpha multiplier all match the spec text.
- Media Queries 4/5: range syntax with reversed/flipped operators, `not`/`and`
  /`or` exclusivity, unknown features evaluate to "unknown" (`MQ_DUNNO`).
- Nesting: `&` replaced by `:is(<parent>)` (correct specificity), relative
  nested selectors, nested declarations after nested rules kept in order.

### What is done well

- Every recursive parser has an explicit depth cap and most have a work
  budget: selectors (48), at-rule nesting (32), `calc()` (depth 64 + 4096
  attempts, thread-local), `var()` expansion (1 MiB output / 100k calls),
  nesting flattener (depth 128 + 16·input + 16 MiB selector budget),
  component values (128), media/container queries (32), colour (24), anchor
  (32), chain matching (1024 frames + 8M ops).
- No `strcpy`/`sprintf` into unchecked buffers anywhere in scope; the few
  fixed buffers (`unit[8]`, `kw[16]`, `name[32]`, `marker[96]`) are all
  guarded by length checks or `g_strlcpy`/`g_snprintf`.
- Values are refcounted and copy-on-write (`ns_css_value_cow`), declaration
  blocks are interned by text hash, and cached sheets are owned by the cache
  (`sh->cached`) so `ns_css_stylesheet_free` cannot double-free them.
- Parse-time `@media` evaluation is made safe by keying every sheet cache on
  the viewport and dropping caches on colour-scheme / reduced-motion / dppx /
  print changes.
- `@import` cycles and fan-out are bounded in `engine.c` by a per-root `seen`
  set plus `NS_CSS_IMPORT_MAX_DEPTH`.
- `attr()` is quoted as a string by default, `type(*)` values are
  syntax-checked, and URL-position use is tainted (modulo CSS-14), matching
  Values 5's threat model.

### Counts by severity

| Severity | Count |
|----------|------:|
| Critical | 0 |
| High     | 0 |
| Medium   | 3 (CSS-1, CSS-2, CSS-3) |
| Low      | 12 (CSS-4 … CSS-15) |
| Info     | 2 (CSS-16, CSS-17) |
| **Total** | **17** |

---

## Security review: `src/js.c` lines 1–36500 (JS/DOM binding, part A)

Tree: `/home/user/nordstjernen-browser` at `a58ff38`, static review only (no build).
Context files read: `src/js.h`, `src/js_internal.h`, `src/dom.h`, `src/ns_quickjs.[ch]`,
plus the relevant parts of `src/net.c`, `src/ws.c`, `src/engine.c`, `src/dom.c` to confirm
callers and lifetimes. Every finding below was confirmed by reading the surrounding code
unless it is explicitly marked **unconfirmed**.

---

### JSA-1 — Streaming XHR checks CORS against the *requested* URL, not the redirect target

- **Severity:** High
- **Category:** security
- **Location:** `src/js.c:22077-22124` (`ns_xhr_on_chunk`); redirect loop `src/net.c:7211-7300`; chunk emission `src/net.c:2601-2605`, `src/net.c:6672`

```c
char *allow_origin = ns_xhr_raw_header_value(raw_headers, "access-control-allow-origin");
gboolean allowed = cors_allows_credentialed(st->origin_url, st->url,
                                            allow_origin, raw_headers,
                                            ns_xhr_with_credentials(ctx, st->obj));
...
JS_SetPropertyStr(ctx, st->obj, "responseURL", JS_NewString(ctx, st->url));
if (ns_url_same_origin(st->origin_url, st->url))
    JS_SetPropertyStr(ctx, st->obj, "_responseHeaders", JS_NewString(ctx, raw_headers));
...
    JS_SetPropertyStr(ctx, st->obj, "responseText", text);
```

**How it is reached.** Async XHR goes through `ns_net_request_async_streaming`
(`js.c:22735`). In `net.c`, `ns_fetch_sync` follows redirects itself in a loop
(`net.c:7211`, each hop with `follow_redirects = FALSE`), and *every* hop installs the same
thread-local stream (`write_ctx.stream = g_hop_stream`, `net.c:6672`). `ns_net_stream_emit`
(`net.c:2601`) forwards body bytes of any 2xx hop together with *that hop's* raw headers.
Nothing in the redirect loop tells the stream that the chain left the request origin.

`ns_xhr_on_chunk` then validates with `cors_allows_credentialed(st->origin_url, st->url, …)`
where `st->url` is the URL script asked for. `cors_allows_credentialed` (`js.c:9893`)
short-circuits to TRUE when `doc_url` and `resp_url` are same-origin, so the final hop's
`Access-Control-Allow-Origin` is never consulted.

**Trigger.** Page on `https://a.example` does
`xhr.open("GET", "/redirect?to=https://b.example/private"); xhr.send()`, where the
same-origin endpoint 302s to a cross-origin resource that does **not** send CORS headers
(the classic open-redirect + intranet/cookie-authenticated target). `b.example` is fetched
with its own cookies (`cookie_partition` is per hop).

**Consequence.** At readyState 2/3 the page observes `status`, `statusText`, the full raw
response headers of `b.example` (`_responseHeaders`, because `st->url` is same-origin), and
the partial-then-growing `responseText` of the cross-origin body via `progress` /
`readystatechange`. The final `ns_xhr_deliver` (`js.c:21809-21840`) correctly re-checks
against `resp->final_url` and `ns_final_url_connect_blocked`, but by then the data has
already been exposed. This is a same-origin-policy bypass for any site with an open
redirect, and it also bypasses the connect-src / mixed-content re-check that
`ns_xhr_deliver` applies to the final URL. `responseURL` is additionally wrong per spec
(must be the final URL).

**Fix.** Make the stream carry the current hop's URL: add `const char *hop_url` to the chunk
(`ns_net_stream_chunk`) filled from `cur_url`/`resp->final_url` in `ns_fetch_sync`, pass it
through `ns_net_chunk_fn`, and in `ns_xhr_on_chunk` use it instead of `st->url` for
`cors_allows_credentialed`, the `_responseHeaders` same-origin test, `responseURL`, and a
`ns_final_url_connect_blocked` check. Simplest safe alternative: in `ns_fetch_sync`, clear
`g_hop_stream` (or set a `cross_origin_redirected` flag that makes `ns_net_stream_emit`
return early) as soon as `from_origin != to_origin` at `net.c:7262`; the body then arrives
only through `ns_xhr_deliver`, which already does the right check.

---

### JSA-2 — Synchronous XHR pumps the main loop without `in_pump`; re-entrant completion frees `ns_xhr_state` under `ns_xhr_on_chunk`

- **Severity:** High
- **Category:** security (use-after-free) / bug
- **Location:** `src/js.c:22304-22322` (`ns_xhr_pump_until_done`), `src/js.c:22141-22156` (`ns_on_xhr_done`), `src/js.c:22116-22124` (`ns_xhr_on_chunk` tail), `src/js.c:22020` (`ns_xhr_state_free(st)` in `ns_xhr_deliver`)

```c
static void
ns_xhr_pump_until_done(JSContext *ctx, JSValueConst this_val)
{
    ns_js *js = js_from_ctx(ctx);
    GMainContext *mc = js && js->main_context ? js->main_context : NULL;
    ...
    while (g_get_monotonic_time() < deadline) {
        ...
        g_main_context_iteration(mc, TRUE);
    }
```

Compare with the engine's own blocking fetch (`js.c:845-851`), which wraps the nested loop
in `js->in_pump = TRUE … = saved`. `ns_xhr_pump_until_done` does not, so every guard that
keys off `ns_js_page_tasks_held()` / `js->in_pump` (`js.c:754`) is inactive while a sync XHR
blocks: `ns_on_xhr_done` (`js.c:22147`) delivers immediately instead of deferring,
`ns_timer_fire` (`js.c:2138`) fires timers, message tasks run (`js.c:1432`), etc.

**Trigger (UAF).**
1. Page starts an async XHR `A` to a chunked/slow same-origin resource.
2. In `A.onreadystatechange`, when `readyState == 3`, it issues a *sync* XHR `B`
   (`xhr.open(..., false); xhr.send()`).
3. `ns_xhr_on_chunk(stA)` is on the stack at `js.c:22117` (`ns_target_fire_event(... "readystatechange")`).
   `B` enters `ns_xhr_pump_until_done`, which iterates the main context. `A`'s completion
   (`ns_on_xhr_done`, queued by `g_task_return_pointer`) is dispatched; `ns_js_page_tasks_held`
   is FALSE, so `ns_xhr_deliver(stA)` runs to completion and ends with `ns_xhr_state_free(stA)`
   (`js.c:22020`), which frees `st->partial`, `st->url`, and `st` itself.
4. Control returns into `ns_xhr_on_chunk`, which executes
   `if (ns_xhr_gen(ctx, st->obj) == st->gen && st->ctx)`,
   `st->partial->len`, `ns_drain_mutations(st->js)`, `ns_js_budget_pop(st->js, &bg)` on the
   freed block.

The `gen`/`ctx` re-checks after the event (`js.c:22100`, `22118`) protect against
`abort()`/`open()` but not against the state being freed, because they themselves read `st`.
The same window exists when the handler at readyState 2 (`js.c:22099`) does a sync XHR.
The attacker needs only a slow same-origin response (or any server they control that the
page is allowed to reach), so this is reachable from ordinary page script and yields a
heap use-after-free in the renderer.

**Fix.** (a) Set `js->in_pump = TRUE` around the loop in `ns_xhr_pump_until_done` exactly as
`js.c:845-851` does — this also restores spec behaviour (no timers/other XHR callbacks during
sync XHR, see JSA-3) and makes `ns_on_xhr_done` defer through `ns_on_xhr_deliver_idle`.
(b) Independently harden `ns_xhr_on_chunk`: hold a flag in `ns_xhr_state`
(`st->in_chunk`) that `ns_xhr_deliver` checks and, if set, defers via
`ns_on_xhr_deliver_idle`, or ref-count `ns_xhr_state`. Both are cheap and belt-and-braces.

---

### JSA-3 — Timers, rAF-independent idles and other network callbacks run during a synchronous XHR

- **Severity:** Medium
- **Category:** spec-compliance
- **Location:** `src/js.c:22304-22322`, `src/js.c:2138-2141`, `src/js.c:1432`, `src/js.c:14377`

```c
    if (js->in_pump)
        return G_SOURCE_CONTINUE;
    if (ns_engine_in_blocking_fetch() && !idle_expired)
        return G_SOURCE_CONTINUE;
```

Same root cause as JSA-2 but a correctness consequence on its own: while `xhr.send()` on a
sync request blocks, `setTimeout`/`setInterval` callbacks, other XHR `onload`s,
`postMessage` tasks and stream chunks are dispatched (`g_main_context_iteration` with no
`in_pump`), so script observes re-entrant execution the platform forbids (the event loop
must be paused for the sync XHR). Besides being a WPT-visible divergence
(`xhr/send-sync-blocks-async.htm`, `xhr/send-sync-timeout.htm` style tests), it is what makes
JSA-2 exploitable. **Fix:** as JSA-2 (a).

---

### JSA-4 — `postMessage` targetOrigin falls back to the target window's own `location.origin` string — **unconfirmed**

- **Severity:** Low (unconfirmed)
- **Category:** security
- **Location:** `src/js.c:15305-15356` (`ns_window_origin_of`), used by `src/js.c:15637`

```c
    const char *frame_url = frame && js && js->frame_urls
        ? g_hash_table_lookup(js->frame_urls, frame) : NULL;
    if (frame_url) { ... return origin; }
    JSValue loc = JS_GetPropertyStr(ctx, win, "location");
    ...
        JSValue ov = JS_GetPropertyStr(ctx, loc, "origin");
        if (JS_IsString(ov)) { out = g_strdup(s); }
```

When the target is neither the main window nor a frame recorded in `js->frame_urls`, the
origin used to enforce `postMessage(msg, targetOrigin)` is whatever string the target
window's `location.origin` property yields. `ns_url_data_object` (`js.c:20560-20595`) builds
`location`-like objects as plain data properties, so if the frame window's `location` is
such a plain object, frame script can `Object.defineProperty(location, "origin", …)` and
make a strict `postMessage(secret, "https://trusted.example")` from the parent succeed
although the frame is on another origin.

**Why unconfirmed.** The global `location` installation (`js.c:66631`, `29112`) and the exact
set of frames that end up with no `frame_urls` entry (`ns_js_mark_iframe_source`, `js.c:70261`;
removed at `js.c:4910`) are outside the reviewed range; I did not verify whether the fallback
is reachable for a cross-origin, script-bearing frame or whether `location.origin` there is a
non-configurable accessor. **Suggested fix regardless:** never derive an enforced origin from
a script-visible property; when `frame_urls` has no entry, treat the origin as unknown and
drop the message (return `NULL` → no match).

---

### JSA-5 — `ns_xhr_on_chunk` assumes `st->js` survives the `readystatechange` handler — **unconfirmed**

- **Severity:** Low (unconfirmed)
- **Category:** bug
- **Location:** `src/js.c:22116-22124`

```c
    ns_target_fire_event(ctx, st->obj, "readystatechange");
    if (ns_xhr_gen(ctx, st->obj) == st->gen && st->ctx)
        ns_xhr_fire_progress_event(ctx, st->obj, "progress", ...);
    ns_drain_mutations(st->js);
```

`ns_js_reset_runtime_state` (`js.c:65956`) nulls `st->ctx`/`st->js` and frees `st->obj` for
all pending XHRs. If a navigation triggered from the handler (e.g. `location.href = …`)
could run that teardown synchronously, the `ns_xhr_gen(ctx, st->obj)` call would read a
freed `JSValue` and `ns_drain_mutations(NULL)` would dereference NULL. I could not locate the
navigation callback path from within the reviewed range to confirm it is always deferred
(the shell drives navigation through the renderer protocol, which suggests it is). Cheap
hardening: re-test `st->ctx && st->js` before the progress event and the drain, as the
headers branch already does at `js.c:22100`.

---

### What is done well

- **Wrapper/opaque invalidation is systematic.** `ns_make_element` pins wrappers and
  `ns_invalidate_wrapper` (`js.c:4881`) is wired to `ns_node.js_invalidate` from `dom.c:1255`,
  nulling the opaque and scrubbing listener, mutation-observer, focus and frame tables, so a
  DOM method on a freed node sees NULL from `ns_unwrap_element` instead of dangling memory.
- **Host objects are brand-checked.** Every non-DOM native (`ns_hostobj`) carries a kind tag
  and methods go through `NS_HO_THIS(ctx, this, kind)`; I found no `JS_GetOpaque` without a
  matching class/kind check in the reviewed range.
- **Internal transport headers are not forgeable.** The `x-nd-` prefix is in the forbidden
  request-header list, so script cannot set `X-ND-CORS`, `X-ND-Credentials`, `X-ND-Initiator`
  etc. that the network layer trusts.
- **Final delivery re-validates after redirects.** `ns_on_js_fetch_deliver` and
  `ns_xhr_deliver` re-run CORS, connect-src and mixed-content checks against
  `resp->final_url`, and the redirect loop strips sensitive headers on origin change and
  re-preflights CORS requests per hop. (JSA-1 is the one path that skips this.)
- **Async state is detached at teardown.** `ns_js_reset_runtime_state` nulls `ctx`/`js` on
  pending fetch/XHR/abort/WebSocket states and frees their JS handles, and callbacks start with
  `if (!st->ctx) return;`. WebSocket close-in-callback is safe thanks to ws.c's refcounted
  dispatch.
- **Structured clone and worker wire encoding are bounded** (depth 512) and forbidden methods
  (CONNECT/TRACE/TRACK) are rejected at the binding.

### Count by severity

| Severity | Count |
|----------|-------|
| Critical | 0 |
| High     | 2 (JSA-1, JSA-2) |
| Medium   | 1 (JSA-3) |
| Low      | 2 (JSA-4, JSA-5 — both marked unconfirmed) |
| Info     | 0 |

---

## Security review — JS Web-API bindings, part B

Scope: `src/js.c` lines 36500–73030, `src/js_canvas.c`, `src/js_canvas_api.c`,
`src/webcrypto.c` (+ its binding in `src/js.c` 12300–13460, read for context),
`src/idb.c`, `src/wasm.c`, `src/ext.c`, `src/js_webauthn.c`, `src/webaudio.c`,
`src/js_perf.c`, `src/bytecode_cache.c`, `src/db.c`. HEAD a58ff38, static review
only (no build possible). Every finding below was confirmed by reading the
surrounding code; items I could not confirm are marked as such.

---

### JSB-1 — Cross-origin iframe can read/write the embedder's IndexedDB
**Severity:** High  **Category:** security
**Where:** `src/idb.c:118` (`ns_idb_partition`), `src/js.c:72948`
(`ns_js_storage_partition`), `src/js.c:54709-54720`
(`ns_iframe_build_native_globals`), `src/js.c:53452` (deny getter),
`data/js/polyfills.js:137,6102-6123,6243`

```c
/* idb.c:118 — partition is taken from the ns_js, not from the realm */
ns_idb_partition(JSContext *ctx)
{
    ns_js *js = JS_GetContextOpaque(ctx);
    const char *partition = ns_js_storage_partition(js);   /* js->partition_key = top-level origin */
```
```js
// polyfills.js:137 — brand check accepts any object inheriting the prototype
return function (obj) { if (!isPrototypeOf.call(proto, obj)) throw ...; return state; };
// polyfills.js:6243
factoryOf = idlSingletonBrand(IDBFactory.prototype, {});
```

**Trigger.** A cross-origin `<iframe>` is given its own JSContext whose opaque
is the shared `ns_js` (`js.c:54797`). `ns_iframe_build_native_globals` calls
`ns_js_setup_document_interfaces`, which installs `__nd_idb` in the frame
global (`idb.c:1187`); `ns_iframe_run_native_scripts` then evaluates the
polyfill bundle in that frame context, which captures the backend into an
`IDBFactory` class and exposes `IDBFactory` on the frame global. The frame
bootstrap afterwards replaces only the `indexedDB` *getter* with a
`SecurityError` thrower (`js.c:53452`). Every backend function derives the
on-disk partition from `js->partition_key`, which is the top-level document's
origin — frame scope changes `js->current_url` but never `partition_key`.
Frame script therefore runs:

```js
const f = Object.create(IDBFactory.prototype);
IDBFactory.prototype.open.call(f, "victim-db").onsuccess = e => { /* read/write */ };
```
and reads/writes `<data_dir>/nordstjernen/indexeddb/sha256(top-origin)/…`.
The same bypass gives storage to `sandbox="allow-scripts"` (opaque-origin)
frames that are supposed to have none.

**Consequence.** Cross-origin disclosure and tampering of the embedder's
IndexedDB (session tokens, cached API data, etc.) by any third-party frame; sandbox escape for storage.

**Fix.** Make the backend partition-aware per realm: pass the realm's
origin/partition explicitly (e.g. store it as a hidden string on the backend
object created per realm, or look it up via `ns_js_realm_url(js, ctx)` and
treat opaque/sandboxed realms as "no storage"). Do not install `__nd_idb` or
run the IDB polyfill in native (cross-origin / opaque) realms at all, and
brand-check IDB methods with a WeakSet of genuine instances instead of
`isPrototypeOf`.

---

### JSB-2 — Canvas 2D state survives navigations and is keyed by a raw node pointer (cross-document pixel disclosure, JSContext use-after-free)
**Severity:** High  **Category:** security / bug
**Where:** `src/js_canvas.c:698-717` (`ns_canvas_state_for`),
`src/js_canvas.c:3332-3350` (`ns_canvas_get_2d`), `src/js_canvas.c:76-95`
(`ns_canvas_state_reset`), `src/js.c:65956-66115`
(`ns_js_reset_runtime_state`, no canvas cleanup), `src/js.c:66816` (only
teardown), `src/libnordstjernen.c:1485/5489` (one `ns_js` per browser)

```c
/* js_canvas.c:698 */
ns_canvas_state *st = g_hash_table_lookup(js->canvas_states, el);   /* key: ns_node* */
...
if (st && (st->w != w || st->h != h)) {
    ns_canvas_state_reset(st, w, h);   /* -> ns_ctx2d_init_state(st->jsctx, st->ctx2d) */
} else if (!st) { ... g_hash_table_insert(js->canvas_states, (gpointer)el, st); }
/* js_canvas.c:3335 */
if (JS_IsObject(st->ctx2d)) return JS_DupValue(ctx, st->ctx2d);
```

**Trigger.** `js->canvas_states` is destroyed only in `ns_js_free`
(`js.c:66816`). `ns_js_reset_runtime_state` (run on every top-level
navigation) frees orphan nodes, frame contexts (`JS_FreeContext`, `js.c:66109`)
and wrappers but leaves `canvas_states` intact, and the renderer keeps one
`ns_js` for the lifetime of the browser. After navigating from origin A to
origin B, every `ns_node *` key is dangling. When B's parser allocates a
`<canvas>` node at a recycled address (same-size allocations are reused by
glibc malloc, so a page that creates many canvases hits this reliably),
`ns_canvas_state_for` returns A's state: `getContext('2d')` hands B the
previous page's `CanvasRenderingContext2D` object and, when width/height
match (default 300×150), A's pixel surface with `origin_clean == TRUE`, so
`getImageData`/`toDataURL` succeed. If the width differs,
`ns_canvas_state_reset` calls `ns_ctx2d_init_state(st->jsctx, …)` where
`st->jsctx` is the (possibly already `JS_FreeContext`'d) frame context the
canvas was created in (`js_canvas.c:3345`, `st->jsctx = js->ctx` inside a
frame scope).

**Consequence.** Cross-origin disclosure of rendered canvas content from the
previous document (and leaking its CanvasRenderingContext2D object into the
new document), plus a use-after-free of a freed `JSContext` after frame
teardown.

**Fix.** Clear `js->canvas_states` in `ns_js_reset_runtime_state` and remove
an entry whenever its node is freed (hook the node's `js_invalidate`
path), or key the table by the `canvas` wrapper object rather than the raw
node pointer. Store `st->rt` only and run `ns_ctx2d_init_state` with the
context that owns `st->ctx2d` (`JS_GetFunctionRealm`-style lookup) or with
`js->main_realm_ctx`.

---

### JSB-3 — Uncaught errors from cross-origin scripts are not muted (XS-Leak)
**Severity:** Medium  **Category:** security / spec-compliance
**Where:** `src/js.c:67240-67290` (`ns_js_report_error_event`,
`ns_js_report_exception_at`, `ns_js_report_uncaught`), `src/js.c:67828-67960`
(`ns_js_eval`), `src/js.c:69300-69400` (`ns_js_run_script_element`)

```c
/* js.c:67255 */
JS_SetPropertyStr(ctx, ev, "message", JS_NewString(ctx, message ? message : ""));
JS_SetPropertyStr(ctx, ev, "filename", JS_NewString(ctx, filename ? filename : ""));
...
JS_SetPropertyStr(ctx, ev, "error", JS_DupValue(ctx, error));
```

**Trigger.** `ns_js_run_script_element` evaluates any 200 response for a
`<script src>` without CORS (nosniff only blocks when the header is present),
and `ns_js_eval` reports every uncaught exception through
`ns_js_report_uncaught(js, ex, origin)` with the full message, the script URL
and the error object. There is no "muted errors" step (`Script error.`,
filename "", lineno 0) for non-CORS cross-origin scripts as required by HTML
§8.1.3.7.

**Consequence.** `window.onerror` on an attacker page receives QuickJS parse
errors (which embed the offending token text) and runtime `Error` messages of
cross-origin, cookie-authenticated resources — the classic login-state /
content-fragment XS-Leak.

**Fix.** Record per-script whether the response was same-origin or
CORS-approved (`crossorigin` attribute + ACAO check already exists for canvas
tainting in `ns_js_resource_origin_clean`); when not, report `"Script error."`
with empty filename, 0/0 line/col and `error: null`.

---

### JSB-4 — Extension content scripts run in the page realm (no isolated world)
**Severity:** Medium  **Category:** security
**Where:** `src/js.c:72057-72085` (`ns_js_run_content_scripts`),
`src/ext.c:1174-1260` (`ns_ext_content_scripts_for_url`), `src/ext.c:483-520`
(`ns_ext_shim_prelude`), `src/ext.c:308` (`ns_ext_js_storage_op`)

```c
/* js.c:72068 */
JSValue shim = JS_Eval(js->ctx, src, strlen(src), "content-script", JS_EVAL_TYPE_GLOBAL);
JSValue r = JS_IsFunction(js->ctx, shim) ? JS_Call(js->ctx, shim, JS_UNDEFINED, 1, &bridges) : ...
```

**Trigger.** The concatenated extension scripts plus the `browser.*` shim are
evaluated in the page's own JSContext/global. The shim captures only
`Object.keys`, `Array.isArray` and `Promise`; the native storage bridge returns
ordinary promises whose `.then` resolves through the page-visible
`Promise.prototype`. A hostile page (or any page, for `document_idle`
scripts) can pre-patch `Promise.prototype.then`, `Object.prototype`,
`JSON.parse`, `document.createElement`, etc.

**Consequence.** Pages can read values the extension fetches from
`browser.storage.*` (tokens, settings), redirect its DOM writes, or hijack
its logic; `getURL()` also reveals the extension's on-disk `file://` path to
whatever the content script exposes. WebExtensions semantics require an
isolated world sharing only the DOM.

**Fix.** Evaluate content scripts in a dedicated `JSContext` (the realm
machinery used for same-origin iframes already exists), with the page DOM
wrappers re-prototyped into that realm, or at minimum freeze/capture every
intrinsic the shim uses and hand bridge results back via native functions
rather than promises resolved in the page realm.

---

### JSB-5 — Script-initiated top-level navigation to `data:` URLs is allowed
**Severity:** Low  **Category:** security
**Where:** `src/js.c:65736-65752` (`ns_location_target_allowed`)

```c
static const char *const allowed[] = {
    "http:", "https:", "about:", "data:", "mailto:", NULL,
};
```

**Trigger.** `location.href = 'data:text/html,…'` from any script (including
cross-origin frames via the cross-window proxy's `location` setter).
**Consequence.** Full-page `data:` documents are a well-known phishing vector
(spoofed address bar); Chrome/Firefox block top-frame `data:` navigations
initiated by script.
**Fix.** Drop `data:` from the allow-list for top-level navigations
(keep it for frames/resources).

---

### JSB-6 — PerformanceObserver arrays mutated while being iterated
**Severity:** Low  **Category:** bug
**Where:** `src/js.c:66066-66070`, `src/js.c:66793-66799`;
`src/js_perf.c:766-776` (`ns_perf_observer_finalizer`), `src/js_perf.c:955-965`
(`ns_perf_observer_release`), `src/js_perf.c:840-880` (`ns_perf_drain_job`)

```c
/* js.c:66067 */
for (guint i = 0; i < js->perf_observers->len; i++) {
    ns_perf_observer *o = g_ptr_array_index(js->perf_observers, i);
    if (o) ns_perf_observer_release(js->ctx, o);   /* may JS_FreeValue(wrapper) -> finalizer */
}
/* js_perf.c:771 */
g_ptr_array_remove_fast(js->perf_observers, o);
```

**Trigger.** `ns_perf_observer_release` drops the pin on `o->wrapper`; when
that is the last reference the finalizer runs synchronously and
`g_ptr_array_remove_fast` swaps the last element into slot `i`, which the
loop then skips. In `ns_js_free` the skipped observer is finalized later by
`JS_FreeRuntime` with `js->perf_observers` already NULL and `js->ctx`
already passed to `JS_FreeContext`. The drain job iterates the same array
while running user callbacks that can construct/finalize observers.
**Consequence.** Observers leaking across navigations (stay pinned, keep
receiving entries), and a fragile teardown ordering.
**Fix.** Iterate over a copy (`g_ptr_array_copy`) or iterate backwards with
`remove_index`, and null out `o->wrapper`/`js` before freeing contexts.

---

### JSB-7 — WebCrypto import/derive leniencies
**Severity:** Low  **Category:** spec-compliance
**Where:** `src/js.c:12699-12760` (`ns_wc_do_import`), `src/js.c:12473-12530`
(`ns_wc_parse_alg`), `src/webcrypto.c:267-345` (`ns_crypto_import_raw`),
`src/webcrypto.c:1141-1185` (`ns_crypto_ecdh`), `src/webcrypto.c:1212-1245`
(`ns_crypto_hkdf`)

```c
/* webcrypto.c:338 — spki/pkcs8 import never checks the key type against algo */
EVP_PKEY *pkey = ns_crypto_pkey_from_der(format, data, len);
ns_crypto_key *k = ns_crypto_key_new(t, algo, hash, curve, EVP_PKEY_get_bits(pkey), extractable, usages);
/* js.c:12511 — publicExponent longer than 4 bytes is silently truncated */
for (gsize i = 0; i < exp_len; i++) val = (val << 8) | exp_buf[i];
```

Confirmed gaps: JWK `ext`, `key_ops` and `alg` are ignored (a JWK with
`ext:false` can be imported extractable); `importKey('spki'|'pkcs8')` accepts an
RSA key for `ECDSA` (operations later fail with `OperationError`, not
`DataError`); `deriveBits` lengths that are not multiples of 8 are truncated
for ECDH/HKDF instead of raising `OperationError`; RSA `publicExponent` >
4 bytes is truncated. None are memory-unsafe (usage/extractable enforcement
in `ns_wc_op_check`/`exportKey`/`wrapKey` is correct); they are conformance
and defence-in-depth issues.
**Fix.** Validate `ext`/`key_ops`/`alg` in `ns_wc_do_import`, compare
`EVP_PKEY_get_base_id(pkey)` with the requested algorithm family, reject
non-byte-multiple lengths, and reject exponents wider than 32 bits.

---

### JSB-8 — Private-mode IndexedDB databases silently discarded by handle LRU
**Severity:** Low  **Category:** bug
**Where:** `src/idb.c:59-85` (`ns_idb_cache_trim`), `src/idb.c:230-270`
(`ns_idb_open_db`), `src/idb.c:150-158` (`ns_idb_path_for_name`)

```c
const char *victim = lru_disk ? lru_disk : lru_mem;
if (victim) g_hash_table_remove(g_idb_handles, victim);   /* unref -> sqlite3_close(":memory:") */
```

**Trigger.** In private mode every database is `:memory:` and lives only in
the handle cache (`refs == 1` between calls). Opening the 17th distinct
`(partition, name)` evicts the LRU in-memory database, destroying its
contents; the next `open()` returns an empty database with version 0.
**Consequence.** Silent data loss for sites using many databases in private
mode. **Fix.** Never trim `:memory:` handles (or key them off a separate
unbounded table owned by the `ns_js`).

---

### JSB-9 — Privileged `__nd*` natives reachable from every realm
**Severity:** Low  **Category:** security
**Where:** `src/idb.c:1187` (`__nd_idb`), `src/js.c:72470-72640`
(`__ndMseAppend`/`__ndMseRemove`/`__ndMseEos` by numeric stream id),
`src/js.c:54716` (installed into cross-origin frame globals)

```c
JS_DefinePropertyValueStr(ctx, global, "__nd_idb", backend, JS_PROP_CONFIGURABLE);
```

The engine's bridge functions are plain configurable globals on page realms,
including native cross-origin frame realms (`ns_js_setup_document_interfaces`
is called for them). `__nd_idb` is deleted by the polyfill only if the
polyfill runs to that point; the MSE bridges take guessable small integer
stream ids and `ns_js` is shared across frames, so a third-party frame can
append/remove media data on the top document's SourceBuffers. Impact is
limited (no memory-safety issue found in the bridges; camera bridges check
`ns_media_origin_granted`), but the attack surface is wide and undocumented.
**Fix.** Keep bridges off the global: pass them to the polyfill as arguments
(as the extension shim already does) and delete them unconditionally; scope
MSE stream ids per realm.

---

### JSB-10 — High-resolution timer and `performance.memory` exposure
**Severity:** Info  **Category:** security (fingerprinting / timing)
**Where:** `src/js_perf.c:195-206` (`NS_TIMER_RESOLUTION_US 100`,
`ns_perf_relative_ms`), `src/js_perf.c:1280-1314`
(`ns_window_performance_memory_get`), `src/js.c:59804`
(`crossOriginIsolated = false`, SharedArrayBuffer deleted)

`performance.now()` is quantised to 100 µs with no jitter; `crossOriginIsolated`
is hard-wired to `false` and `SharedArrayBuffer` is removed from every global,
so no sub-microsecond timers exist (good). `performance.memory` reports real
`JS_ComputeMemoryUsage` figures, a modest fingerprinting surface. Consider
adding jitter and rounding `performance.memory` as Chrome does; otherwise
acceptable.

---

### Unconfirmed / not pursued
- `ns_perf_observer_free` and `ns_wasm_memory_finalizer` dereference
  `js->ctx` / `m->ctx` from finalizers that can run inside `JS_FreeRuntime`
  after `JS_FreeContext(js->ctx)` (`js.c:67142-67143`). Whether a surviving
  object can reach that point depends on GC ordering; not demonstrated.
- `ns_ce_value_free` frees registry JSValues through the process-global
  `ns_active_js()`; a mismatch would need two live `ns_js` in one process
  (headless and GTK paths both create exactly one), so not confirmed.

### What is done well
- Every IndexedDB SQL statement uses bound parameters; the SQLite handle is
  hardened (`SQLITE_DBCONFIG_DEFENSIVE`, no extensions, DQS off, `NOFOLLOW`,
  0600 files, per-origin SHA-256-hashed directories, page-count quotas).
- The bytecode cache is in-memory only, keyed by SHA-256 of
  `kind\0origin\0line:col:flags\0<full source>`, so no on-disk poisoning and
  no cross-source aliasing; `JS_READ_OBJ_BYTECODE` is never used on persisted
  data (IDB values are read with `JS_READ_OBJ_REFERENCE` only).
- WebCrypto: usages and extractability are enforced before every operation
  (`ns_wc_op_check`, `exportKey`, `wrapKey`), HMAC verify uses `CRYPTO_memcmp`,
  AES-GCM tag lengths are whitelisted and ciphertext shorter than the tag is
  rejected, AES-CTR counter wrap is handled, randomness comes from
  `RAND_bytes`.
- Canvas: `origin-clean` propagates through `drawImage`, patterns, ImageBitmap
  and OffscreenCanvas, and is checked in `getImageData`, `toDataURL`, `toBlob`
  and `convertToBlob`; all pixel loops clip to both surfaces and sizes are
  capped at 32767 with `size_t` arithmetic.
- WebAssembly bindings cap module/memory sizes, detach `memory.buffer` on
  every growth path and guard `ArrayBuffer.prototype.transfer*` against wasm
  memories; externrefs are reference-counted through the runtime.
- Frame isolation is thoughtfully layered: X-Frame-Options / frame-ancestors /
  frame-src / sandbox CSP tokens, nesting depth limits, cross-origin window
  proxies, muted cookie access for opaque-origin frames, module MIME-type and
  SRI/nosniff checks on scripts.

### Count by severity
- Critical: 0
- High: 2 (JSB-1, JSB-2)
- Medium: 2 (JSB-3, JSB-4)
- Low: 5 (JSB-5, JSB-6, JSB-7, JSB-8, JSB-9)
- Info: 1 (JSB-10)

---

## Nordstjernen layout / paint / animation review

Scope: `src/layout.c`, `src/layout.h`, `src/paint.c`, `src/anim.c`, `src/render.c`,
`src/engine.c`, `src/engine.h`, `src/print.c` (plus the parts of `src/css.c`,
`src/js.c` and `src/dom.c` needed to confirm or discard findings). HEAD a58ff38.
Static review only; no build. Every finding below was confirmed by reading the
callers and the clamps that precede the cited code, except where marked
**unconfirmed**.

Severity count: **Critical 0 · High 1 · Medium 3 · Low 3 · Info 1** (8 findings).

---

### LAY-1 — Exponential layout time for nested inline-level atomic containers

- **Severity:** High
- **Category:** security (denial of service of the renderer process)
- **Where:** `src/layout.c:10118-10122` (`inline_atomic_needs_layout`),
  `src/layout.c:10575-10616` (`measure_inline_atomics_begin`),
  `src/layout.c:7676-7687` (`inline_box_measure_cacheable`),
  `src/layout.c:11067-11076` (`atomic_inline_fit_width`),
  `src/layout.c:17424-17444` (`layout_block` shrink-to-fit of atomic containers)

```c
static gboolean
inline_atomic_needs_layout(const ns_box *ab)
{
    if (!ab) return FALSE;
    return !(g_abs_ph_set && g_hash_table_contains(g_abs_ph_set, ab));
}
...
        double basis = inline_atomic_measure_basis(ab);
        if (inline_atomic_needs_layout(ab))
            layout_box(ab, basis, parent_style);
        if (max_content && shrinks)
            widen_atomic_to_max_content(ab, parent_style, basis, &saved);
        if (to_min_content) {
            double w = min_width_of(ab, parent_style);
```

**Trigger.** `<div style="display:inline-block">` nested *d* levels deep (one
child per level, any text leaf). Each level that is laid out shrink-to-fit
calls `atomic_inline_fit_width`, which calls `measure_natural_width` and (if
the natural width overflows) `min_content_width_of`. Each of those walks the
inline run and, for every atomic child, calls `layout_box(ab, …)`
unconditionally (`inline_atomic_needs_layout` is TRUE for every atomic that
is not an abs-pos placeholder), and the min-content path additionally calls
`min_width_of(ab)`. The measure cache is explicitly bypassed when the run
contains atomics (`inline_box_measure_cacheable` returns FALSE if
`inline_atomics->len > 0`). The final `layout_box` of the parent lays the
child out a third time. There is no "already laid out at this basis" memo on
the child, so the work per level multiplies: roughly 3 layouts of the child
per layout of the parent, i.e. **O(3^d)** total `layout_box` calls. 20 nested
inline-blocks already means on the order of 3.5 billion leaf layouts; nothing
caps `d` short of `NS_LAYOUT_MAX_DEPTH 512`. Nested flex containers were also
checked and are *not* affected because `layout_flex_row` reuses the first
pass when `same_input && cross_preset` (12903-13136).

**Consequence.** A single hostile page (or an ordinary page with deeply nested
inline-block widgets) pins the sandboxed renderer at 100 % CPU for an
unbounded time; the shell shows a hung tab. No memory corruption.

**Fix.** Memoise intrinsic widths per atomic box for the current layout pass
(e.g. store `measured_min/natural` plus the `basis` and a pass counter on
`ns_box`, cleared in `ns_layout_build`), and skip `layout_box` in
`measure_inline_atomics_begin` when the box was already laid out for the same
basis in this pass. Alternatively let `inline_box_measure_cacheable` accept
runs with atomics once the atomics themselves cache their contribution.

---

### LAY-2 — Non-finite CSS lengths (`infinity`, `nan`, overflowing literals) reach layout unsanitised

- **Severity:** Medium
- **Category:** security / bug
- **Where:** `src/css.c:3101-3113` (`parse_length`), `src/css.c:3617-3621`
  (calc constants), `src/layout.c:27-69` (`length_resolve`)

```c
    double v = g_ascii_strtod(text, &end);
    if (!end || end == text) return FALSE;
    *out_v = v;
...
        static const struct { const char *name; gsize len; double val; } consts[] = {
            { "infinity", 8, INFINITY }, { "pi", 2, G_PI }, { "e", 1, G_E },
            { "nan", 3, NAN },
        };
...
    if (v->kind == NS_CSS_V_CALC)
        return v->u.calc.pct / 100.0 * basis + v->u.calc.px;
```

**Trigger.** `width: 1e999px`, `padding: calc(infinity * 1px)`,
`margin-left: calc(nan * 1px)`, `text-indent: calc(infinity * 1px)`.
`g_ascii_strtod` returns ±HUGE_VAL for overflowing literals and `parse_length`
accepts it; the calc constants produce ±Inf and NaN by design (CSS Values 4
§10.9 requires them to be *clamped at used-value time*, which never happens
here). `length_resolve` returns the raw double; `layout_block`,
`edges_from_style` (587) and `resolve_used_height` (155) store it straight
into `content_width/height`, `margin`, `padding`. `ns_css_calc_math_fn_px`
propagates NaN through min()/max()/clamp() as well.

**Consequence.** Inf/NaN in box geometry propagates to every consumer:
`(int)(content_width * NS_PANGO_SCALE)` (LAY-3) is UB on Inf/NaN;
`control_pad_spaces` (layout.c:2350-2360) computes `(int)((pad - space)/space
+ 0.5)` with pad=Inf; paint computes surface sizes from `ceil(w)`. Paint has
*partial* NaN guards (`paint_walk`, sticky/snap) but layout has none, so
behaviour is implementation-defined at best. Confirmed that integer
properties (`order`, `z-index`, `column-count`) are *not* affected because
they go through `css_integer_clamp` (css.c:11530-11563), and that shadow blur
/ spread are clamped at parse time (css.c:5935-5936).

**Fix.** In `parse_length` reject (or clamp to ±1e7) non-finite results; in
`calc_unit_value` / `ns_css_calc_math_fn_px` clamp the final value to a finite
range per CSS Values 4 §10.9; as defence in depth make `length_resolve` return
`fallback` for `!isfinite()` results and clamp magnitudes to e.g. ±(1<<24) px.

---

### LAY-3 — Out-of-range double→int conversions for Pango units (undefined behaviour)

- **Severity:** Medium
- **Category:** bug (UB; platform-dependent garbage width)
- **Where:** `src/layout.c:8287`, `8295`, `8523`, `8740`;
  `src/paint.c:4001`, `4557`, `7707`; `src/layout.c:2358`

```c
        ns_pango_layout_set_width(layout, (int)(content_width * NS_PANGO_SCALE));
...
        if (ti > 0) ns_pango_layout_set_indent(layout, (int)(ti * NS_PANGO_SCALE));
...
    int n = (int)((pad - space) / space + 0.5);
```

**Trigger.** Any content width above `G_MAXINT / 1024 ≈ 2.1e6` px, e.g.
`<div style="width:3000000px">text</div>`, or any Inf/NaN from LAY-2. No
clamp precedes these casts: `layout_block` resolves `width` with no magnitude
limit, and inline layout multiplies by `NS_PANGO_SCALE` (1024) before the
cast. C11 §6.3.1.4 makes the conversion undefined; on x86-64 it yields
`INT_MIN`, which Pango treats as "no wrapping" or as negative width, so the
text is laid out as one infinitely long line while the box thinks it has a
positive width.

**Consequence.** Mis-shaped text, wrong line counts in `line_heights` arrays
(indices are guarded, so no overread), and formally UB that
`-fsanitize=float-cast-overflow` would flag. Not exploitable on its own.

**Fix.** Add a single helper `static int px_to_pango(double px) { if (!(px
== px)) return -1; return (int)CLAMP(px, -G_MAXINT / NS_PANGO_SCALE,
G_MAXINT / NS_PANGO_SCALE) * NS_PANGO_SCALE; }` in a shared header and use it
at all eight sites; clamp `pad` in `control_pad_spaces` before the division.

---

### LAY-4 — Explicit grid hard-capped at 24 tracks

- **Severity:** Medium
- **Category:** spec-compliance (CSS Grid 1 §7.2, §7.2.3, §8.3)
- **Where:** `src/css.h:565` (`NS_CSS_TRACKS_MAX 24`), `src/css.c:5534`,
  `5584` (`parse_tracks`), `src/layout.c:14594` (`expand_auto_repeat_ex`),
  `14824` (`grid_resolve_line_from`), `16076` (`layout_grid`)

```c
#define NS_CSS_TRACKS_MAX 24
...
    if (n < 0) n = n_tracks + 2 + n;
    if (n < 1 || n > NS_CSS_TRACKS_MAX + 1) return 0;
    return (int)n;
```

**Trigger.** `grid-template-columns: repeat(30, 1fr)` or any explicit
template with more than 24 tracks; `grid-column: 26 / 27`;
`repeat(auto-fill, 20px)` on a 1000 px wide container (50 columns).

**Consequence.** Tracks past the 24th are silently dropped, items placed on
lines > 25 fall back to auto-placement (`return 0`), and auto-fill/auto-fit
stop at 24 repetitions. Grid §7.2 imposes no track limit and §7.2.3 requires
auto-fill to repeat "as many times as will fit". Real pages (calendars with
31 columns, data tables, icon grids) render with wrapped/misplaced items.
Security-wise the cap is good (fixed arrays stay in bounds everywhere they are
indexed: `grid_span_accommodate`, occupancy); the issue is purely conformance.

**Fix.** Make `ns_css_tracks.tracks` a growable `GArray` (or raise the
constant to something like 1000 and keep the clamps), size `col widths`/
`occupancy` dynamically as rows already are (`NS_GRID_ROWS_MAX 4096`), and
keep an upper bound (e.g. 10 000 tracks × 4096 rows) to preserve the DoS
protection.

---

### LAY-5 — `paint_adopted_abs_boxes` is quadratic in sibling count

- **Severity:** Low
- **Category:** bug (performance)
- **Where:** `src/paint.c:10817-10846`; callers `src/paint.c:11102`, `11288`

```c
    if (!box_groups_contents(e)) return FALSE;
    gboolean any = FALSE;
    for (const ns_box *p = e->parent; p; p = p->parent) {
        for (const ns_box *c = p->first_child; c; c = c->next_sibling) {
            abs_escape esc;
            abs_box_escape(c, p, &esc);
            if (!esc.owner) continue;
```

**Trigger.** N sibling boxes that each "group contents" (any `opacity`,
`filter`, `transform`, `clip-path`, `mask`, `mix-blend-mode`, etc.), e.g. 5 000
`<div style="opacity:.9">`. For each such box `paint_walk` calls this twice
(the `cr == NULL` probe at 11102 and the paint at 11288); each call scans
*all* siblings of every ancestor up to the nearest positioned/moving one,
running `abs_box_escape` (which itself walks the sibling's abs-pos
descendants) on each.

**Consequence.** O(N²) `abs_box_escape` calls per frame — tens of millions
for a few thousand grouped siblings — i.e. a slow or hung paint on pages that
otherwise lay out quickly. No memory-safety impact.

**Fix.** Pre-compute, once per paint, a map `owner → list of escaped abs
boxes` (a single pass over the tree, same information `abs_box_escape`
derives), and have `paint_adopted_abs_boxes` look `e` up in it.

---

### LAY-6 — Blurred box-shadow may allocate and triple-blur a 268 MB surface per shadow

- **Severity:** Low
- **Category:** bug (resource exhaustion)
- **Where:** `src/paint.c:694-722` (`paint_blurred_box_shadow`),
  `src/paint.c:854-906` (`paint_inset_box_shadow`)

```c
    int radius = (int)(blur * 0.5 + 0.5);
    if (radius < 1) radius = 1;
    if (radius > 256) radius = 256;
    int pad = radius * 3 + 2;
    int isw = (int)ceil(sw), ish = (int)ceil(sh_h);
    ...
    int surf_w = isw + pad * 2, surf_h = ish + pad * 2;
    if (surf_w > 8192 || surf_h > 8192) return;
    cairo_surface_t *surf = blurred_shadow_surface(&key, pad, surf_w, surf_h);
```

**Trigger.** `box-shadow: 0 0 500px #000` (blur clamped to [0,1000] at parse
time, css.c:5935) on a box of ~6500×6500 px, repeated with distinct sizes or
colours so the blur cache misses — e.g. 100 such boxes each one pixel
different. The 8192 guard caps a *single* surface at 8192² × 4 B = 268 MB and
three box-blur passes over it, but per-shadow allocation is not budgeted
across a page and the cache key (`sw, sh, radii, colour, radius`) makes
identical-looking shadows of slightly different size all miss.

**Consequence.** Multi-GB transient allocation and seconds of blur per frame
in the renderer; cairo surface creation failure is handled
(`cairo_surface_status` is checked), so a failed allocation is graceful
rather than a crash.

**Fix.** Budget total shadow surface pixels per paint (e.g. 64 M px) and fall
back to the unblurred path past the budget; or render oversized shadows at a
downscaled resolution (blur is scale-invariant: draw at 1/4 size with
radius/4 and scale up), which is what `drop_shadow_surface` effectively does
with its 4096 limit.

---

### LAY-7 — `colspan`/`rowspan` clamped to 100 instead of 1000 / 65534

- **Severity:** Low
- **Category:** spec-compliance (HTML §4.9.11 "Processing model" for `td`/`th`)
- **Where:** `src/layout.c:1947-1949` (`build_cell`)

```c
    const char *cs_attr = ns_element_get_attr(n, "colspan");
    if (cs_attr) cell->colspan = ns_parse_int(cs_attr, 1, 1, 100);
    const char *rs_attr = ns_element_get_attr(n, "rowspan");
    if (rs_attr) cell->rowspan = ns_parse_int(rs_attr, 1, 1, 100);
```

**Trigger.** `<td colspan="120">` or `<td rowspan="500">`.

**Consequence.** HTML requires `colspan` to be clamped to [1,1000] and
`rowspan` to [0,65534] (with 0 meaning "to the end of the row group"). Cells
spanning more than 100 columns stop short and the table layout shifts; the
`rowspan=0` form is treated as 1. The table code already sizes its arrays
dynamically up to `NS_TABLE_MAX_COLS 4096`, so the larger limits are safe.

**Fix.** `ns_parse_int(cs_attr, 1, 1, 1000)` and `ns_parse_int(rs_attr, 1, 0,
65534)`, then implement `rowspan=0` as "span to end of row group" in
`layout_table`'s `rs_remain` bookkeeping.

---

### LAY-8 — Animation state keyed by raw `ns_node *` outlives node removal (unconfirmed)

- **Severity:** Info
- **Category:** security (potential UAF) — **unconfirmed**
- **Where:** `src/anim.c:325-340` (`ns_anim_prune`), `src/anim.c:1142` (only
  caller, inside `ns_anim_observe_all`), `src/js.c:8666-8674`
  (`ns_js_orphan_node`)

```c
    g_hash_table_iter_init(&it, a->states);
    while (g_hash_table_iter_next(&it, &key, &val)) {
        if (g_hash_table_contains(live, key)) continue;
        ns_anim_state *s = val;
        ...
        g_hash_table_iter_remove(&it);
    }
```

**Why it was examined.** `a->states` and `anim_emit`'s event queue hold
`const ns_node *` with no reference count (`ns_node` has none, `dom.h`). States
are dropped only when the next full style observe pass runs. Between a DOM
removal and that pass, `ns_anim_tick` may emit `animationend`/`transitionend`
events that `ns_js_dispatch_anim_events` (js.c:36302-36306) targets at the
stored pointer.

**Why it is unconfirmed.** Every removal path I traced (`removeChild`,
`replaceChild`, `innerHTML` reset, `remove()`) parks the detached subtree in
`js->orphan_nodes` (js.c:8672, 8693, 8750, 37266, 37640, 37694 …) rather
than freeing it, and the sweeps that free orphans (js.c:66074-66076,
66940-66957) were not read in their calling context to establish that they
always run *after* `ns_anim_observe_all` has pruned. If any sweep can run
between a removal and the next observe pass, the stored pointer dangles and
the next tick is a use-after-free. Non-JS removal paths (navigation tears
down the whole `ns_anim` with the document, so that one is safe) were not
found.

**Suggested hardening regardless.** Call `ns_anim_prune`-style removal from
the DOM detach hook (a `ns_anim_forget_node(anim, node)` invoked by
`ns_js_orphan_node` and by the orphan sweep), so animation state never
outlives the node it describes irrespective of pass ordering.

---

### Spec-compliance notes (no finding filed)

- **Block formatting / margin collapsing (CSS 2.1 §8.3.1):** `collapsed_margin`
  (layout.c:16891-16896) correctly combines mixed-sign margins (max positive +
  min negative) and the parent/first-child and parent/last-child collapse
  honours padding/border separation. Correct.
- **Floats (§9.5.1):** float placement (17597-17715) respects rules 1-7
  (no higher than preceding floats, shift down on overflow, clear). Correct.
- **Flexbox §9.7:** `flex_resolve_lengths` (12500-12551) freezes inflexible
  items, loops violation checks and distributes remaining space; min/max
  violations handled. Correct.
- **Grid §12.7.1 fr sizing:** `grid_flex_track_sizes` (14263-14303) computes
  the hypothetical fr with `MAX(flex_sum, 1.0)`, matching the spec's "if the
  sum is less than 1, treat as 1". Correct.
- **Grid §8.3 negative lines:** `grid_resolve_line_from` maps `-1` to
  `n_tracks + 1`. Correct (subject to LAY-4's cap).
- **Table auto layout (CSS 2.1 §17.5.2.2):** `layout_table` distributes
  spanning cells' min/max evenly over the spanned columns and distributes
  excess proportionally to max-content; `docs/CSS-compatibility.md` already
  calls this an approximation. Acceptable, no fix requested.
- **Multicol (CSS Multicol §3.4 pseudo-algorithm):** `ns_css_used_column_count`
  (css.c:1262-1294) follows the spec's used-width/count formulas; balancing
  is capped at 256 iterations, which is a reasonable engineering bound.

### What is done well

- Recursion depth is capped everywhere it matters: `NS_LAYOUT_MAX_DEPTH 512`,
  `g_build_block_depth`, `g_inline_collect_depth`, `g_contents_depth`,
  `NS_GRID_NESTING_MAX 64`, `dom_tree_order_cmp`'s 128-deep chains, and
  `ns_box_free` is iterative, so hostile DOM depth cannot blow the stack.
- Every `cairo_image_surface_create` in `paint.c` and `engine.c` is followed by
  a `cairo_surface_status` check, and raster sizes are clamped before creation
  (4096 / 8192 px sides, 16 M px backdrops, `NS_ENGINE_PNG_MAX_SIDE 30000`,
  blur sigma ≤ 512, ≤ 4096 textures per 3-D quad batch).
- Grid and table bookkeeping is bounds-checked: `grid_occupied`/`grid_occupy`,
  rows capped at 4096, columns at `NS_TABLE_MAX_COLS`, `table_collapse_borders`
  and `inline_merge_prefix` guard `G_MAXSIZE` multiplication overflow.
- `process_absolute_boxes` copies each `ns_abs_entry` by value before calling
  into `build_block`, which may append to `g_abs_pending`, avoiding the classic
  dangling-`GArray`-element bug.
- Integer-valued properties (`order`, `z-index`, `column-count`, shadow blur
  and spread) are clamped at parse time, so the layout casts of those values
  are safe.
- `anim.c` bounds active animations (`NS_ANIM_MAX_ACTIVE 256`) and clamps
  iteration counts before casting (`iter_d > 1e9`), and `print.c` caps spans
  (`NS_PRINT_MAX_SPANS`) and pages (4096).

### Count by severity

| Severity | Count | IDs |
|----------|-------|-----|
| Critical | 0 | — |
| High | 1 | LAY-1 |
| Medium | 3 | LAY-2, LAY-3, LAY-4 |
| Low | 3 | LAY-5, LAY-6, LAY-7 |
| Info | 1 | LAY-8 (unconfirmed) |

---

## Security review — GPU and media subsystems (Nordstjernen, HEAD a58ff38)

Static review only (no build possible in this environment). Scope: `src/webgl.c`,
`src/webgpu.c`, `src/glsl_validate.c`, `src/glctx.c`, `src/video.c`,
`src/video_decode.c`, `src/video_mp4.c`, `src/mp4.c`, `src/media_os*.c`,
`src/audio/main.c`, `src/camera.c`, the sample-buffer side of `src/webaudio.c`,
plus the call sites into the vendored pl_mpeg / minimp3 and the shell's
`X-Audio` relay in `src/gtk/procview.c`. Every finding below was confirmed by
reading the surrounding code; where a claim depends on behaviour I could not
execute (driver, wgpu-native, glslang) it is marked **unconfirmed** with the reason.

Severity scale: Critical / High / Medium / Low / Info.
Category: security | bug | spec-compliance.

---

### GPU-1 — WebGL draws accept client-side attribute pointers; no-program draws skip vertex-range validation (Windows/WGL)

**Severity:** High  **Category:** security
**Where:** `src/webgl.c:2694-2707` (`wgl_vertexAttribPointer`), `src/webgl.c:850-854` (`wgl_program_attrib_mask`), `src/webgl.c:925-958` (`wgl_attribs_cover`), `src/glctx.c:88-96`

```c
    GLintptr offset = (GLintptr)argi(ctx, argc, argv, 5);
    if (size < 0 || stride < 0 || offset < 0) return JS_UNDEFINED;
    glVertexAttribPointer(index, size, type, norm, stride,
                          (const void *)offset);
```
```c
    glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
    if (prog <= 0) return 0;          /* mask 0 => wgl_attribs_cover() checks nothing */
```

**Trigger / consequence.** WebGL requires `vertexAttribPointer` to fail with
`INVALID_OPERATION` when no `ARRAY_BUFFER` is bound; this implementation forwards
the call, so with buffer 0 bound GL records a *client* pointer whose address is
the page-chosen `offset`. The only guard before a draw is `wgl_attribs_cover`,
which derives the set of attributes to check from the current program and
returns TRUE when no program is current. On the Windows path `glctx.c` creates
the context with `wglCreateContextAttribsARB` and no profile mask, i.e. a
compatibility-profile context, where `glDrawArrays` with program 0 is legal
(fixed-function) and generic attribute 0 aliases the vertex position. A page can
therefore make the driver read vertex data from an arbitrary 31-bit address in
the renderer process; the fetched values are rendered and can be read back with
`readPixels`. On Linux (GLES 3, draw without program is an error) and macOS (core
profile rejects client arrays) the same code is only a spec violation. Robust
buffer access does not help: it covers buffer objects, not client memory.

**Fix.** In `wgl_vertexAttribPointer`/`wgl_vertexAttribIPointer` reject the call
(`INVALID_OPERATION`) when `GL_ARRAY_BUFFER_BINDING` is 0 (or when
`g->bound_array_buffer == 0`). In every draw entry point reject when
`GL_CURRENT_PROGRAM == 0` (WebGL mandates `INVALID_OPERATION` there). On Windows
request a core-profile context (`WGL_CONTEXT_CORE_PROFILE_BIT_ARB`) so client
arrays are impossible by construction.

---

### GPU-2 — WebGPU canvas readback submits an unvalidated command buffer (wgpuQueueSubmit abort path still reachable)

**Severity:** High  **Category:** security (process abort / DoS)
**Where:** `src/webgpu.c:4932-4945` (`ns_webgpu_canvas_surface`), `src/webgpu.c:2344-2356` (`wg_ctx_ensure_target`), `src/webgpu.c:2404-2409` (`configure` usage), `src/webgpu.c:1709-1717` (`GPUTexture.destroy`), `src/webgpu.c:383-406` (`requiredLimits`)

```c
    WGPUCommandEncoder enc = wgpuDeviceCreateCommandEncoder(c->device, NULL);
    ...
    wgpuCommandEncoderCopyTextureToBuffer(enc, &src, &dst, &copy_size);
    WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(enc, NULL);
    wgpuQueueSubmit(c->queue, 1, &cmd);
```

**Trigger / consequence.** `docs/webgpu.md` and `wg_encoder_finish`/`wg_queue_submit`
correctly treat `wgpuQueueSubmit` of a command buffer that failed validation as a
fatal (abort) path in wgpu-native, and guard the page-driven submit accordingly.
The compositor's own readback does not: it encodes a copy and submits without an
error scope or any check that the copy is valid. The page controls every input
to that copy:

* `ctx.getCurrentTexture().destroy()` destroys the very texture `c->target`
  the next paint copies from (same handle, only AddRef'd), so the copy fails
  validation.
* `configure({usage: GPUTextureUsage.STORAGE_BINDING})` with `bgra8unorm`
  (or an incompatible `viewFormats` list) makes `wgpuDeviceCreateTexture`
  return an *error* texture; `wg_ctx_ensure_target` only tests for NULL.
* `requestDevice({requiredLimits:{maxBufferSize: 1024}})`, or an 8192x8192
  `rgba16float` canvas (512 MiB > default `maxBufferSize`), makes the readback
  buffer creation fail; `c->readback` is a non-NULL error buffer.
* `device.destroy()` while a configured canvas is pending.

In each case the next paint submits an invalid command buffer and, per the
project's own documented wgpu-native behaviour, the renderer process aborts.

**Fix.** Wrap the readback in `wgpuDevicePushErrorScope(Validation)` /
`wg_pop_internal_scope` exactly like `wg_encoder_finish`, skip the submit on
error, and drop the surface. Additionally validate `configure()` usage/viewFormats
against the format before creating the target, reject readback buffers larger
than the device's `maxBufferSize`, and mark the context unconfigured after
`device.destroy()`.

---

### GPU-3 — `getMappedRange()` on a buffer whose creation failed reaches the fatal `wgpuBufferGetMappedRange`

**Severity:** High  **Category:** security (process abort / DoS)
**Where:** `src/webgpu.c:876-897` (`wg_device_createBuffer`), `src/webgpu.c:767-806` (`wg_buffer_getMappedRange`)

```c
    desc.usage = (WGPUBufferUsage)(usage & ~0x3FFu ? 0 : usage);
    desc.mappedAtCreation = mapped ? 1 : 0;
    WGPUBuffer wbuf = wgpuDeviceCreateBuffer(d->device, &desc);
    if (!wbuf) return JS_ThrowTypeError(ctx, "createBuffer: wgpu returned null");
    ...
    if (desc.mappedAtCreation) { b->mapped = TRUE; b->map_offset = 0; b->map_size = desc.size; }
```

**Trigger / consequence.** wgpu-native returns a non-NULL *error* buffer when a
descriptor fails validation (usage 0, size not a multiple of 4 with
`mappedAtCreation`, size above `maxBufferSize`, ...) and reports the error
asynchronously through the uncaptured-error sink. The wrapper nevertheless marks
the buffer as mapped. `getMappedRange()` then passes the wrapper's own checks
(offset 0, size within `map_size`) and calls `wgpuBufferGetMappedRange`, which
the code comments and `docs/webgpu.md` describe as "fatal on any error"; wgpu-core
rejects the call with `InvalidResource` before looking at the range, so the
process aborts. One JS statement
(`device.createBuffer({size: 6, usage: 0, mappedAtCreation: true}).getMappedRange()`)
is enough.

**Fix.** Validate the descriptor locally before `wgpuDeviceCreateBuffer` (usage
non-zero and within the known bits, size ≤ `maxBufferSize`, size % 4 == 0 when
`mappedAtCreation`), or create the buffer inside an internal validation error
scope and refuse to set `b->mapped` when the scope reports an error.

---

### GPU-4 — `queue.onSubmittedWorkDone()` leaves a wgpu callback pointing at a dead stack frame

**Severity:** Medium  **Category:** security (stack memory write after return)
**Where:** `src/webgpu.c:582-607`

```c
    wg_work_wait wait = { 0 };
    ...
    ci.userdata1 = &wait;
    wgpuQueueOnSubmittedWorkDone(q->queue, ci);
    for (int i = 0; i < 4000 && !wait.done; i++)
        wgpuInstanceProcessEvents(ns_webgpu_instance());
    return wg_promise_resolved(ctx, JS_UNDEFINED);
```

**Trigger / consequence.** The callback (`wg_on_work_done`) writes
`((wg_work_wait *)u1)->done = 1` unconditionally. The loop polls non-blocking
(no `wgpuDevicePoll(…, wait=1)`) at most 4000 times — a few milliseconds. A
submission that takes longer (a heavy compute dispatch or large draw submitted
just before) is still in flight when the function returns, so the registered
callback keeps `&wait`. The next `wgpuInstanceProcessEvents`/`wgpuDevicePoll`
(from `mapAsync`, `popErrorScope`, or the canvas readback) fires it and writes
a `1` into whatever now occupies that stack slot. Page-controlled timing, limited
payload; still a genuine lifetime bug in native code.

**Fix.** Heap-allocate the wait record and free it from the callback, or poll
the device with `wgpuDevicePoll(device, 1, NULL)` until `done` as `mapAsync`
does. The same pattern (`&wait` on the stack) is used by `requestAdapter`,
`requestDevice`, `popErrorScope` and `mapAsync`; those happen to be immediate or
poll with wait in wgpu-native, but the heap-allocated form would remove the
dependency on that detail.

---

### GPU-5 — WebGL 2 element-array shadow goes stale when a PBO write lands in the same buffer

**Severity:** Medium  **Category:** security (bypass of the index-range check; relies on driver robustness)
**Where:** `src/webgl.c:3572-3581` (`readPixels` PBO path), `src/webgl.c:2908-2923` (`wgl_draw_elements_ok`), `src/webgl.c:2592-2600` (`bindBuffer`)

```c
    if (pack_buffer) {
        ...
        glReadPixels(x, y, w, h, format, type, (void *)(intptr_t)offset);
        return JS_UNDEFINED;          /* no wgl_elem_shadow_clear() */
    }
```

**Trigger / consequence.** Index-range validation (`wgl_elem_max_index`) runs on
a CPU shadow of the element buffer that is refreshed on `bufferData`,
`bufferSubData`, `copyBufferSubData` and transform feedback. `readPixels` into a
`PIXEL_PACK_BUFFER` also writes GPU-side into a buffer but does not invalidate
the shadow, and the WebGL 2 rule that a buffer is either an element buffer or a
non-element buffer (`bindBuffer` must raise `INVALID_OPERATION` on a mismatch)
is not enforced. A page can bind one buffer to both targets, fill it with
indices via `readPixels` from a framebuffer it drew into, and then `drawElements`
with a shadow that still says "all indices are 0". `wgl_attribs_cover` therefore
accepts vertex buffers far too small for the real indices. On Linux/Windows the
robust-access context turns this into zero reads; on macOS (no robustness,
documented) it becomes an out-of-bounds GPU read of other VRAM visible through
rendering.

**Fix.** Call `wgl_elem_shadow_clear(g, pack_buffer)` in the PBO `readPixels`
path, and enforce the WebGL 2 element/non-element buffer-type restriction in
`bindBuffer`/`bindBufferBase`/`bindBufferRange`.

---

### GPU-6 — Shader validator bounds bracket nesting but not expression depth

**Severity:** Medium  **Category:** security (renderer stack exhaustion) — **unconfirmed by execution**
**Where:** `src/glsl_validate.c:208-216`, `src/webgl.c:2278` (4 MiB cap)

```c
        if (c == '{' || c == '(' || c == '[') {
            if (++depth > NS_GLSL_MAX_NESTING) { ... return; }
        }
```

**Trigger / consequence.** The pre-scan caps `{([` nesting at 256 but places no
limit on chain depth that needs no brackets: `- - - - … x`, `! ! ! … b`,
`a + a + a + … + a` or `c ? 1 : c ? 1 : …` up to the 4 MiB source cap produce an
AST of depth ~10^6. glslang's parser builds that tree and its link/validation
passes walk it recursively (`TIntermTraverser`), as does the driver compiler that
receives the same source afterwards. Both are expected to exhaust the stack and
crash the renderer. Not executed here, hence unconfirmed; the recursive traversal
in glslang is well known.

**Fix.** Count consecutive unary operators and binary-operator chain length in
`ns_glsl_scan_source` (a few thousand is generous), or lower the source cap for
the validator and reject sources whose token count exceeds a budget.

---

### GPU-7 — Contexts hold a raw `ns_node *canvas` with no invalidation hook

**Severity:** Medium  **Category:** bug / security (potential use-after-free) — **trigger unconfirmed**
**Where:** `src/webgl.c:38`, `331-339` (`ns_webgl_sync_size`), `5882`; `src/webgpu.c:95`, `2336-2337`; `src/js.c:4881-4915` (`ns_invalidate_wrapper`); `src/dom.c:817-826`

```c
    guint32 gen = g->canvas ? g->canvas->attr_gen : 0;   /* webgl.c:333 */
    ...
    int w = ns_webgl_dim(g->canvas, "width", 300);
```

**Trigger / consequence.** `ns_webgl` and `ns_wg_context` keep the canvas node
pointer for the object's whole life and dereference it on every API call and on
every paint (`g_webgl_by_node` / `g_webgpu_ctx_by_node` are keyed by it). When a
node is freed, `ns_invalidate_wrapper` detaches the JS wrapper and several
JS-side tables but never reaches `webgl.c`/`webgpu.c`. The normal removal path
(`ns_js_sweep_orphans`, js.c:70479-70510) only frees subtrees without a live
wrapper, and the context object pins the canvas wrapper (`g->canvas_obj`), so
that path is safe. Direct-free paths exist, though: `ns_node_set_editable_value`
(dom.c:817-826) frees all children of a contenteditable host without consulting
wrappers, and `ns_element_clear_children` is used when no `ns_js` is attached.
A canvas with a live context placed inside a contenteditable host that is then
edited would leave `g->canvas` dangling; the next `gl.*` call reads freed memory.
I could not follow the editing path end-to-end (outside scope), so the trigger is
unconfirmed; the missing hook itself is confirmed.

**Fix.** Give `ns_node` a release callback list (or extend `js_invalidate`) and
have `webgl.c`/`webgpu.c` register to clear `g->canvas`, remove the
`*_by_node` entry and mark the context lost. Treat a NULL `canvas` as "lost" in
`wgl_cur`/`ns_webgl_canvas_surface`.

---

### GPU-8 — `bufferData` records the requested size even when GL rejected the allocation

**Severity:** Low  **Category:** bug (weakens later range checks)
**Where:** `src/webgl.c:2623-2646`

```c
        glBufferData(target, (GLsizeiptr)size, NULL, usage);
        ...
        wgl_set_buffer_size(g, bn, (size_t)size);
```

**Trigger / consequence.** `wgl_buffer_size` is the oracle for `bufferSubData`,
`getBufferSubData`, `drawElements` and `wgl_attribs_cover`. If `glBufferData`
fails (`OUT_OF_MEMORY` for a 1 GiB request, or an invalid target/usage) the GL
store keeps its old size while the tracker says 1 GiB, so a following
`getBufferSubData`/`bufferSubData` passes the wrapper's check and the result
depends on the driver's own validation (mandatory in core GL, so not memory
unsafe, but the documented "doesn't trust the driver" property is lost).

**Fix.** Check `glGetError()`/`GL_BUFFER_SIZE` after `glBufferData` and record
the actual size.

---

### GPU-9 — WebGL objects are not tied to their owning context; `getContext` ignores the requested version

**Severity:** Low  **Category:** spec-compliance
**Where:** `src/webgl.c:614-620` (`wgl_name`), `src/webgl.c:5855-5857`

```c
    ns_webgl_obj *o = JS_GetOpaque(v, ns_webgl_obj_class_id);
    return o ? (int)o->name : 0;
```

**Trigger / consequence.** A `WebGLBuffer` created on context A is accepted by
context B and acts on whatever object has the same integer name there (WebGL
requires `INVALID_OPERATION`). `ns_webgl_get_context` returns the existing
context for any `webgl`/`webgl2` request on the same canvas, so
`getContext('webgl2')` after `getContext('webgl')` hands back a WebGL 1 object
instead of `null`. No memory impact (names are plain integers, contexts are not
shared), but it confuses the per-context trackers (`buffer_sizes`, `elem_data`)
and the compressed-format and extension gating.

**Fix.** Store the owning `ns_webgl *` in `ns_webgl_obj` and reject mismatches;
compare the stored `version` in `ns_webgl_get_context`.

---

### GPU-10 — `ArrayBuffer.prototype.transfer()` on a mapped range defeats `unmap()`/`destroy()`

**Severity:** Info  **Category:** bug (leak, not a UAF)
**Where:** `src/webgpu.c:704-722` (`wg_buffer_detach_ranges`), `src/quickjs/quickjs.c:64065-64094`

**Trigger / consequence.** `transfer()` moves the data pointer *and* the
`realloc_func/opaque` (the `held` GPUBuffer reference) into a new ArrayBuffer
and detaches the old one. `wg_buffer_detach_ranges` then sees the old buffer
already detached, sets `range_escaped`, and permanently refuses to unmap or
destroy. The mapping and the GPUBuffer leak for the page's lifetime; the `held`
reference prevents a UAF, so this is memory growth only.

**Fix.** Mark the ArrayBuffer non-transferable, or track the live backing pointer
rather than the ArrayBuffer value.

---

### GPU-11 — A "lost" context still executes GL calls

**Severity:** Info  **Category:** spec-compliance
**Where:** `src/webgl.c:519-530` (`wgl_cur`), `src/webgl.c:1916-1927`

**Trigger / consequence.** `WEBGL_lose_context.loseContext()` only flips
`g->lost`; `wgl_cur` does not consult it, so every method keeps issuing GL
commands while `isContextLost()` is true. The spec requires every call to be a
no-op (and `getError` to report `CONTEXT_LOST_WEBGL`). Harmless for memory
safety; noted because the docs present context loss as implemented.

---

### MED-1 — Audio-helper file allow-list is lexical; symlinks in the cache directory are not resolved

**Severity:** Low  **Category:** security — **partly unconfirmed** (depends on whether the renderer sandbox permits `symlink()` in its cache dir)
**Where:** `src/gtk/procview.c:864-885` (`pv_stream_url_allowed`), `src/audio/main.c:1346-1363` (`local_path_for`), `src/audio/main.c:327-344` (`read_file`)

```c
    char *canon = g_canonicalize_filename(path, NULL);
    char *streams = g_build_filename(g_get_user_cache_dir(), "nordstjernen",
                                     subdir, "", NULL);
    gboolean inside = g_str_has_prefix(canon, streams);
```

**Trigger / consequence.** The shell only forwards `open`/`reload` URLs that are
`file://` paths under `~/.cache/nordstjernen/msaudio/` (good — the helper's own
http/https/data support is unreachable from a page). The check is lexical:
`g_canonicalize_filename` collapses `..` but does not resolve symlinks. The
renderer writes into that directory; if a compromised renderer can create a
symlink there, the helper (which runs outside the renderer sandbox, and whose
own Linux sandbox grants it its temp dir) will `fopen`+decode any file the user
can read. The helper only returns `meta <duration>`/`pos`/`error` lines, so the
leak is limited to "does it decode" and the decoded length — a weak oracle, not
content disclosure. Decoding also goes through libav/minimp3/pl_mpeg on bytes
the attacker fully controls (already the case for legitimate files).

**Fix.** Resolve with `realpath()` before the prefix test, and open with
`O_NOFOLLOW` in the helper (or `fstat` and require a regular file owned by the
user with the expected directory as parent).

---

### MED-2 — OS decoder geometry (display aperture / crop) is trusted when copying frames

**Severity:** Low  **Category:** security (OOB read if the OS decoder reports inconsistent geometry) — **unconfirmed**: depends on the platform decoder honouring its own invariants
**Where:** `src/media_os_mf.c:272-283, 495-507, 629-633`; `src/media_os_android.c:116-119, 183-203`

```c
            if (SUCCEEDED(IMFMediaType_GetBlob(t, &MF_MT_MINIMUM_DISPLAY_APERTURE, ...)) &&
                sz == sizeof area && area.Area.cx > 0 && area.Area.cy > 0) {
                d->vis_width = (int)area.Area.cx;
                d->vis_height = (int)area.Area.cy;
```
```c
            size_t need = (size_t)stride * (size_t)d->out_height * 3 / 2;
            if (cur >= need)
                ok = ns_os_frame_copy_nv12(out, base, stride, base + ..., stride,
                                           d->vis_width, d->vis_height);
```

**Trigger / consequence.** The visible size comes from the MFT's reported
aperture (derived from the attacker-supplied SPS cropping) and is only checked
to be positive, never `<= out_width/out_height`; the buffer-size check uses the
coded size while the copy uses the visible size. The Android path likewise
copies `crop_right-crop_left+1` columns and `crop_bottom-crop_top+1` rows from a
buffer validated against `stride*slice_height` without checking the crop against
them. If the OS decoder ever reports an aperture/crop larger than the coded
frame, `ns_os_frame_copy_nv12/i420` reads past the sample buffer. Microsoft's and
Android's decoders normally clamp, which is why this is Low/unconfirmed.

**Fix.** Clamp `vis_*`/crop to the coded size in `vdec_pick_output`,
`vdec_read_format` and `vdec_copy`, and reject frames where the check fails.

---

### MED-3 — `stsz` sample count is trusted for fixed-size tracks

**Severity:** Low  **Category:** bug (memory DoS)
**Where:** `src/mp4.c:255-267` (`build_samples`)

```c
    uint32_t fixed = rd32(tab->stsz.p + 4);
    uint32_t n = rd32(tab->stsz.p + 8);
    if (!fixed && n > (tab->stsz.len - 12) / 4) n = ...;
    if (n == 0 || n > NS_MP4_MAX_SAMPLES) return;
    ...
    t->samples = calloc(n, sizeof *t->samples);
```

**Trigger / consequence.** With `sample_size != 0` the count is bounded only by
`NS_MP4_MAX_SAMPLES` (2,000,000), so a few hundred bytes of `moov` allocate
~2 M × sizeof(ns_mp4_sample) per track, for up to 16 tracks, and
`ns_mp4_probe_chunk_range` re-parses the init segment for every MSE append.
Bounded, but large relative to the input. The rest of the demuxer is tight:
box sizes 0/1/64-bit handled, every table bounded by its span, `trun` counts
bounded by `per` or 65536, sample offsets re-checked against the buffer in
`video_mp4.c:134`.

**Fix.** For fixed-size tracks also bound `n` by `nchunks × max(samples_per_chunk)`
from `stsc`, or lower `NS_MP4_MAX_SAMPLES` for the probe path.

---

### MED-4 — MSE memory is capped per stream kind but not per page

**Severity:** Info  **Category:** bug (memory DoS)
**Where:** `src/video.c:29, 209, 1539-1541`

**Trigger / consequence.** Each `ns_mse_stream` may hold 256 MiB of video plus
256 MiB of audio bytes, the active window copies up to 96 MiB more, and the
decoder copies the window again (`ns_lav_open`/`ns_mp4_player_new` memcpy).
Streams are keyed by a page-chosen id with no cap on their number, so a page can
create many `MediaSource`s and append to each. The per-kind caps and the
window-trim logic are good; a per-document total would complete them.

---

### What is done well

* **Pixel-transfer bounds are exact and overflow-checked.** `wgl_transfer_bytes`
  reproduces GL's unpack/pack arithmetic (row length, skips, image height,
  alignment) with `__builtin_*_overflow`, never under-estimates for any
  format/type combination I could construct, and `view_bytes` re-validates
  typed-array offset/length against the live `ArrayBuffer` (resizable buffers
  included). Compressed uploads require the exact block-formula size.
* **Index-buffer validation beyond the spec.** The CPU shadow of element
  buffers plus `wgl_attribs_cover` gives real vertex-range checking that most
  browsers delegate to ANGLE; transform-feedback writes and `copyBufferSubData`
  invalidate it correctly.
* **Cross-origin texture tainting is enforced** through the same
  `ns_js_drawimage_source_surface` path as 2D canvas (SecurityError on a
  non-origin-clean source) for both WebGL and WebGPU `copyExternalImageToTexture`.
* **The audio helper boundary is well designed.** The shell parses the `X-Audio`
  side channel itself, restricts `open`/`reload` to `file://` paths under the
  renderer's `msaudio` cache dir, rejects CR/LF and over-long lines, and the
  helper decodes into bounded buffers (256 MiB input, 200 M floats output),
  whitelists libav formats, refuses nested I/O (`io_open`), and sandboxes itself
  on Linux. GStreamer pipeline strings are built only from registry factory
  names, never from page data.
* **pl_mpeg integration is safe.** The vendored decoder is locally patched to
  pad the frame planes for half-pel reads, dimensions are 12-bit (no overflow),
  the sequence header is parsed once so a later header cannot resize frames
  under the renderer's fixed BGRA buffer, and the renderer bounds width/height
  at 16384 before allocating.
* **WebGPU wrapper already closes many wgpu-native abort paths** (unknown
  usage bits, zero copy strides, empty error-scope pops, duplicate/consumed
  command buffers in `submit`, mapped-range validation) and reads JS arrays into
  private copies before touching handles, so getters cannot race the native
  calls.

### Count by severity

| Severity | Count | IDs |
|---|---|---|
| Critical | 0 | — |
| High | 3 | GPU-1, GPU-2, GPU-3 |
| Medium | 4 | GPU-4, GPU-5, GPU-6 (unconfirmed by execution), GPU-7 (trigger unconfirmed) |
| Low | 5 | GPU-8, GPU-9, MED-1 (partly unconfirmed), MED-2 (unconfirmed), MED-3 |
| Info | 3 | GPU-10, GPU-11, MED-4 |

Total: 15 findings (11 GPU, 4 media).

---

## Nordstjernen — security review of the application shell and persistence layers

Scope: `src/gtk/{procwindow,appmain,pagelayers,permissions,pointerlock,sessionstore,titlerow,icons,ns_webauthn,webauthn_win}.c`, `src/gtk/macos_dock.m`, `src/libnordstjernen.c`, `src/headless.c`, `src/headless_main.c`, `src/{config,bookmarks,history,capability,i18n,win_launcher,embed_shim,debuglog}.c`, `data/nordstjernen.desktop`, `data/org.nordstjernen.WebBrowser.metainfo.xml`, `data/safebrowsing.list` + `src/safebrowsing.c`, `scripts/*.sh`, `subprojects/*.wrap`.
Tree: `/home/user/nordstjernen-browser` at `a58ff38`, static review only (no build possible). Every finding below was confirmed by reading the code paths named; where a cross-file dependency mattered (`src/js.c`, `src/net.c`, `src/renderer_serve.c`, `src/gtk/procview.c`, `src/security.c`) the relevant lines were read too.

---

### APP-1 — `window.open()` bypasses the file:/privileged-page navigation gate
**Severity:** High  **Category:** security
**Where:** `src/js.c:27638-27646` (op emitted without policy check), `src/gtk/procwindow.c:1815-1836` (shell opens any scheme), `src/renderer_serve.c:843-857` (fresh renderer has no referrer), `src/net.c:4515` and `src/net.c:5169` (empty `top_url` is treated as "from chrome").

```c
/* js.c:27638 — popup path: resolve and emit, no browser_allows_navigation_url() */
g_autofree char *abs_url = *url ? ns_url_resolve(page, url) : g_strdup("about:blank");
...
JS_SetPropertyStr(ctx, op, "url", JS_NewString(ctx, abs_url));
ns_popup_emit(ctx, op);

/* procwindow.c:1834 — shell: permission check only, then load whatever URL */
NsProcView *popup = proc_window_add_tab_opener(pw, url, TRUE, ns_proc_view_is_private(v), ...);
```

**Trigger.** The engine deliberately refuses web→`file:` and web→`about:settings|config|history` navigations in `browser_allows_navigation_url()` (`libnordstjernen.c:933-944`), and `window.open(url, "_self")` goes through that check via `nav_cb`. But the popup branch (`ns_popup_open`) emits the resolved URL straight to the shell. `pw_popup_open()` applies only the pop-ups *permission* (default **allow**) and `normalize_url()` keeps `file:`/`about:` prefixes verbatim (`procwindow.c:483-487`). The popup is a brand-new renderer process; its `/open` handler derives `referrer`/`initiator` only from `s->cur` (`renderer_serve.c:843-849`), which is NULL in a fresh process, so `browser_open_common()` fetches with `top_url == NULL`. `ns_file_access_allowed()` returns TRUE for an empty `top_url` (`net.c:4515`) and `about_request_from_chrome()` does the same (`net.c:5169`). Net effect: after one click on `https://evil.example`, `window.open("file:///home/USER/Downloads/evil.html")` or `window.open("about:settings")` opens that page in a new tab, with `window.opener` linkage (`opener_token`/`opener_origin` are forwarded, `procwindow.c:1835-1836`). The shell even marks the load `user_activated=TRUE` (`procview.c:3061-3064`).

**Consequence.** (a) A privileged `about:settings` tab can be summoned by web content, contradicting the 403 the engine returns for the same page when fetched. (b) An attacker who first gets `evil.html` downloaded (any non-"program" extension saves with no prompt) and can guess the Downloads path can open it as a `file:` document that is allowed to `fetch()` every file in its own directory and below (`net.c:4521-4524`) and send the contents back through the opener/popup relay (`pw_popup_handle` → `"post"` from popup to opener is permitted). The username is the only unknown.

**Fix.** Run the popup URL through the same policy as navigation: in `ns_popup_open()` reject (or fall back to `nav_cb`) when `browser_allows_navigation_url()` would refuse it — simplest is a `popup_allowed_cb` set from `libnordstjernen.c`. Defence in depth in the shell: `pw_popup_open()` should accept only `http:`, `https:`, `about:blank`, `data:`, `blob:` unless the opener page itself is `file:` (mirroring the download check at `procwindow.c:1503-1508`). In `renderer_serve.c /open`, when `opener_origin` is present and no referrer exists, use it as the `top_url`/initiator so `ns_file_access_allowed()` and `about_request_from_chrome()` see a remote initiator.

---

### APP-2 — Popup ↔ opener message `origin` is renderer-asserted and relayed unverified
**Severity:** Medium  **Category:** security
**Where:** `src/gtk/procwindow.c:1772-1781`, `src/gtk/procwindow.c:1836`

```c
static const char *const fields[] = { "token", "to", "data", "origin",
                                      "target", "path", "ports", "id" };
for (gsize i = 0; i < G_N_ELEMENTS(fields); i++) {
    const char *val = rj_get_str(op, fields[i]);
    ...
    rj_append_str(out, val);          /* "origin" copied verbatim */
}
...
linked ? rj_get_str(op, "origin") : NULL);   /* opener_origin from the opener renderer */
```

**Trigger.** The shell is the trust boundary between the two renderer processes of an opener and its popup. For `"post"`, `"call"` and `"port"` ops it copies the `origin` field the *sending* renderer wrote, and for `"open"` it forwards the opener's self-declared `origin` as the popup's `opener_origin`. Honest renderers fill these from `ns_popup_realm_origin()`, but a renderer compromised through any engine bug can set `origin` to e.g. `https://bank.example` when posting to a popup it opened on that site.

**Consequence.** `event.origin` checks in the receiving page (OAuth/payment popup flows do exactly `if (event.origin !== "https://…") return;`) are defeated from another process — a cross-tab, cross-origin escalation the per-tab renderer design is meant to prevent. Impact is bounded because a compromised renderer already has wide cookie/network reach (SECURITY.md "known gaps"), hence Medium rather than High.

**Fix.** Have the shell stamp `origin` itself from the sending tab's `ns_proc_url_origin(ns_proc_view_url(v))` (and `opener_origin` from `page_origin`, which `pw_popup_open` already computes). If frame-originated messages must keep their own origin, at least require the claimed origin to be the tab's top-level origin or be delivered with a `frame=true` marker the receiver can see.

---

### APP-3 — Download staging check and move are not atomic (symlink can land in ~/Downloads)
**Severity:** Low  **Category:** security
**Where:** `src/gtk/procwindow.c:844-889`; `src/security.c:916-917` (renderer seccomp allows `symlink`/`symlinkat`)

```c
GFile *f = g_file_new_for_path(path_canon);
GFileType type = g_file_query_file_type(f, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL);
...
return type == G_FILE_TYPE_REGULAR;              /* T1 */
...
gboolean ok = g_file_move(src, dst, G_FILE_COPY_NOFOLLOW_SYMLINKS, ...);   /* T2 */
...
ns_security_mark_download_origin(d->path, final_url ...);  /* setxattr follows symlinks */
```

**Trigger.** The renderer owns the staging directory and may create symlinks. Between the regular-file check (T1) and the `rename` (T2) it can swap the `.part` file for a symlink; `rename` moves the symlink itself into `~/Downloads/<name>`. `setxattr()` on Linux then follows it and tags the *target*.

**Consequence.** A compromised renderer plants `~/Downloads/report.pdf → ~/.ssh/id_rsa`-style links; a later "Open" or an upload of that "download" exposes the target. Requires a compromised renderer and a race, so Low. (Exploitability of the race window is unconfirmed; the code path is confirmed.)

**Fix.** `open(O_RDONLY|O_NOFOLLOW)` the staging file, `fstat()` it, and move by fd (`linkat(fd, "", dst, AT_EMPTY_PATH)` / `renameat2`), or after the move `lstat()` the destination and unlink it if it is not a regular file before marking it.

---

### APP-4 — Address bar decodes percent-escaped URL delimiters
**Severity:** Low  **Category:** security (URL display)
**Where:** `src/gtk/procwindow.c:516-538`

```c
char *dec = g_uri_unescape_string(url, NULL);
...
if (c < 0x20 || c == 0x7f || (c >= 0x200e && c <= 0x200f) ||
    (c >= 0x202a && c <= 0x202e) || (c >= 0x2066 && c <= 0x2069)) { ... return g_strdup(url); }
return dec;
```

**Trigger.** Only control and bidi characters veto decoding; `%2F %3F %23 %40 %3A %25 %20` are all turned into `/ ? # @ : % ` and shown. `https://evil.example/login%40bank.example%2Fsession%3Fid=1` is displayed as `https://evil.example/login@bank.example/session?id=1`, and `%25xx` sequences display as a different escaped URL than the one loaded.

**Consequence.** The displayed URL's structure no longer matches the real one; combined with a long path it is mild spoofing material. The host stays leftmost and IDN is shown as Punycode, so Low.

**Fix.** Keep the delimiter set reserved (`gen-delims`/`sub-delims` and `%`) escaped when decoding for display, as Chromium's `UnescapeRule::NORMAL` does, or decode only the path/query *after* re-escaping those characters.

---

### APP-5 — Executable-download warning list misses common dangerous types; dialog shows the download URL for `data:`/`blob:`
**Severity:** Low  **Category:** security
**Where:** `src/gtk/procwindow.c:1036-1051`, `src/gtk/procwindow.c:1225-1229`

```c
static const char *const exts[] = {
    ".exe", ".msi", ".msix", ".bat", ".cmd", ".com", ".scr", ".pif",
    ...
    ".jar", ".py", ".pl", ".rb", ".php", ".apk", ".xpi", ".crx",
};
```

**Trigger.** Not flagged (saved silently, then openable from the Downloads window via the default handler): `.application`/`.appref-ms` (ClickOnce), `.msp`, `.mst`, `.scf`, `.iso`/`.img`/`.vhd`/`.vhdx`, `.chm`, `.ws`/`.wsc`/`.sct`, `.pyw`, `.diagcab`, `.settingcontent-ms`, `.library-ms`, `.ps1xml`/`.psm1`/`.psd1`, macOS `.terminal`/`.workflow`. Separately, for a `data:`/`blob:` download the confirmation detail prints the whole URL where the host should be (`host && *host ? host : url`).

**Consequence.** Users get no "program file" prompt for these; the `data:` case produces an unreadable prompt.

**Fix.** Extend the list (Chromium's `download_file_types.asciipb` is a good reference) and, for non-network schemes, show the *page* origin in the dialog.

---

### APP-6 — UI catalogue, Safe Browsing list and icon theme fall back to CWD-relative paths
**Severity:** Low  **Category:** security
**Where:** `src/i18n.c:142`, `src/safebrowsing.c:407-409`, `src/gtk/procwindow.c:132-139`

```c
/* i18n.c:142 */  return catalogue_path_in("data/i18n", lang);
/* safebrowsing.c:407 */ if (g_file_test("data/safebrowsing.list", G_FILE_TEST_EXISTS)) load_file("data/safebrowsing.list");
```

**Trigger.** When no bundled copy is found next to the executable, both loaders read `data/…` relative to the process's current directory; the exe-relative search also climbs to `../../../../data/i18n`. Catalogue values replace every UI string, including the text of the permission bar, the download "Save this program file?" dialog and the fullscreen notice.

**Consequence.** A browser launched from an attacker-writable directory (an extracted archive, a shared scratch dir) gets attacker-chosen prompt wording and an attacker-chosen blocklist. This sits at the edge of the threat model (local write access), hence Low.

**Fix.** Drop the CWD fallbacks (keep `NS_I18N_DIR`/`NS_SAFEBROWSING_LIST` for developers), and limit the exe-relative search to the installed layout plus the build tree's `../data` and `../../data`.

---

### APP-7 — Tab label truncation splits UTF-8 sequences
**Severity:** Low  **Category:** bug
**Where:** `src/gtk/procwindow.c:1425-1428`

```c
const char *t = text && *text ? text : ns_i18n("Untitled");
char *clip = g_strndup(t, 40);
if (label) gtk_label_set_text(GTK_LABEL(label), clip);
```

**Trigger.** Any page title whose 40th byte falls inside a multi-byte character (CJK, emoji, accented Latin).

**Consequence.** Invalid UTF-8 reaches Pango, which warns on every redraw and truncates the label. Cosmetic, but page-triggerable log noise.

**Fix.** Clip at a character boundary (`g_utf8_offset_to_pointer`) or rely on the label's `max_width_chars`/ellipsize that is already set.

---

### APP-8 — `pl_texture()` size check can wrap on 32-bit `size_t`
**Severity:** Low  **Category:** bug
**Where:** `src/gtk/pagelayers.c:131-144`

```c
size_t row = (size_t)width * 4u;
if (!map || width <= 0 || rows <= 0 || off > map_size ||
    (size_t)rows * row > map_size - off)
    return NULL;
GBytes *bytes = g_bytes_new(map + off, (size_t)rows * row);
```

**Trigger.** `width` and `rows` come from the renderer's layer description via `sscanf("%d")`. On 32-bit targets `rows * row` wraps (e.g. 65536 × 16384 × 4) so the bound passes with a tiny product; on 64-bit the product of two `INT_MAX` values also wraps.

**Consequence.** `gdk_memory_texture_new()` re-validates the byte count against the stride and rejects the texture with a critical, so this is a robustness bug, not a memory-safety one (unconfirmed beyond GDK's check).

**Fix.** Reject `width > map_size / 4` and `rows > (map_size - off) / row` before multiplying.

---

### APP-9 — Renderer may silently change some settings without a prompt
**Severity:** Info  **Category:** security
**Where:** `src/config.c:496-500`, `src/gtk/procwindow.c:1380-1392`

```c
static const char *const unprompted_keys[] = {
    "speculative_preload", "async_image_decode", "images_enabled",
    "cache_cap_mb", "window_width_px", "window_height_px", "layout_viewport_px",
};
```

A compromised renderer's `X-Config` proposal that touches only these keys is applied and written to `nordstjernen.conf` immediately (`ns_config_apply_text`). Disabling images or setting `cache_cap_mb` to 0 is a nuisance, not an escalation; documented in SECURITY.md as a residual gap. Consider dropping `images_enabled` and `cache_cap_mb` from the allow-list.

---

### APP-10 — Permission decisions for IPv6-literal origins are never persisted
**Severity:** Info  **Category:** bug
**Where:** `src/gtk/permissions.c:368-373`

```c
return origin && strstr(origin, "://") && !strchr(origin, '\n') &&
       !strchr(origin, '[') && !strchr(origin, ']');
```

`ns_proc_url_origin()` serialises IPv6 hosts as `https://[::1]`, so Allow/Block for such sites lives only in memory and silently reverts at restart. Encode the group name (e.g. percent-encode `[`/`]`) instead of refusing.

---

### APP-11 — WebAuthn `clientDataJSON.origin` uses the renderer's spelling
**Severity:** Info  **Category:** security
**Where:** `src/gtk/ns_webauthn.c:289-290`, `src/gtk/ns_webauthn.c:339-341`

```c
gboolean same = page_origin && g_ascii_strcasecmp(page_origin, origin) == 0;
...
op->call.client_data = client_data_json(create, rj_get_str(options, "challenge"), origin);
```

The shell correctly refuses a request whose origin differs from the page shown, but the comparison is case-insensitive and the *renderer's* string is what gets signed. A relying party compares `origin` exactly, so a differently-cased value only causes a failed login, not a forged one. Use `page_origin` in `client_data_json()` to make the shell the sole author, as `docs/webauthn.md` says it is.

---

### APP-12 — sessionStorage of every non-private tab is written to disk
**Severity:** Info  **Category:** privacy
**Where:** `src/gtk/procwindow.c:3541`, `src/gtk/sessionstore.c:288-291`, `src/libnordstjernen.c:1573-1599`

```c
tab->storage = (char *)ns_proc_view_session_storage(v);   /* serialised sessionStorage */
...
sqlite3_bind_text(st, 6, tab->storage, -1, SQLITE_TRANSIENT);
```

Session restore persists `sessionStorage` (which sites treat as tab-lifetime, often holding tokens) into `~/.config/nordstjernen/session.db` every second a tab changes. Firefox does the same, the file is `0600`/`0700`, and private tabs are excluded, so this is a documentation point: mention it in `docs/privacy-policy.md` ("site storage … page cache") or clear the `storage` column at a clean exit.

---

### APP-13 — Windows debug log is on by default and `NS_LOG_FILE=` (empty) picks a path outside the profile
**Severity:** Info  **Category:** privacy
**Where:** `src/debuglog.c:236-258`

```c
#ifdef G_OS_WIN32
    return !g_getenv("NS_NO_LOG_FILE");
#else
    return g_getenv("NS_LOG_FILE") != NULL;      /* "" counts as set */
#endif
...
g_dlog_file_path = g_build_filename(g_get_user_data_dir(), "Nordstjernen", "nordstjernen-debug.log", NULL);
```

On Windows every warning/error — including `download: cannot move %s to %s`, which names the site-chosen file — is appended to `%LOCALAPPDATA%\Nordstjernen\nordstjernen-debug.log` (documented in `docs/Windows.md`). On Unix an empty `NS_LOG_FILE` enables logging to `~/.local/share/Nordstjernen/` (capital N), a directory outside the Landlock allow-list and the profile. No URLs or cookies were found in warn/error paths. Treat `""` as unset and use `NS_APP_DIR_NAME`.

---

### What is done well

- **Persistence is injection-safe.** Every SQL statement in `history.c`, `sessionstore.c` is prepared with bound parameters; `bookmarks.txt` strips tabs/newlines on write; `config.c` strips `\r\n` from string values, parses only known keys, and shows renderer-proposed changes in a dialog with control/bidi characters replaced and values truncated.
- **Downloads are handled carefully.** The shell never parses response bytes; staging paths are canonicalised and pinned to the cache directory; names are reduced to a leaf, stripped of control/bidi characters, Windows-reserved names and trailing dots/spaces, capped at 200 bytes; MOTW/xattr/quarantine is applied; `file:` targets are allowed only from `file:` pages.
- **Capability prompts are anchored to the shell's own view of the page.** `pv_capability_requested` refuses origins that do not match the current URL, the bar text is fixed UI text plus that origin, and Downloads/Pop-ups derive the origin from `ns_proc_view_url()` rather than from the renderer.
- **WebAuthn shell layer** validates same-origin, secure context, non-IP host, PSL-based RP-ID suffix, and builds `clientDataJSON` itself; `webauthn.dll` is loaded with `LOAD_LIBRARY_SEARCH_SYSTEM32`.
- **Supply chain hygiene.** All wraps (`wgpu-native-*`, `simde`) carry `source_hash`; `quickjs.wrap` pins a commit; `scripts/fetch-wgpu-native.sh` and `scripts/build-ffmpeg-lgpl.sh` verify SHA-256 and refuse unpinned versions; no `curl | sh` anywhere in `scripts/`.
- **No translated string is ever used as a format string** (`ns_i18n()` results only appear as `%s` arguments or literal labels), and the Windows launcher builds an absolute path from its own module directory with proper argument quoting.

### Count by severity

| Severity | Count | IDs |
| --- | --- | --- |
| Critical | 0 | — |
| High | 1 | APP-1 |
| Medium | 1 | APP-2 |
| Low | 6 | APP-3, APP-4, APP-5, APP-6, APP-7, APP-8 |
| Info | 5 | APP-9, APP-10, APP-11, APP-12, APP-13 |

---

## Nordstjernen — web-standards conformance review (HTML / CSS Syntax 3 / Selectors 4)

Repository: `/home/user/nordstjernen-browser` at `a58ff38` (read-only static review; no build available).
Scope A: every HTML/CSS string the browser ships and renders itself. Scope B: tokenizer / selector / tree-builder-consumption conformance in `src/css_syntax.c`, `src/css_grammar.c`, `src/css_prop_syntax.c`, `src/css.c`, `src/html.c`, `src/html_lexbor.c`, plus a mapping of the largest failing WPT clusters in `docs/wpt-subtests.tsv` to code.

Every finding below was confirmed by reading the cited code. Nothing was run. Items I could not pin down are marked **unconfirmed**.

Security headline: **no unescaped untrusted data reaches a privileged page.** Every URL, host, file name, error string and title interpolated into `about:` / error / listing / viewer pages goes through `ns_html_escape_text` (`src/html.c:91-120`, escapes `& < > "`) or `g_markup_escape_text` (`& < > " '`); every attribute context is double-quoted; the history store only records `http(s)` URLs without control characters (`src/history.c:29-37`), so no `javascript:` link can land on the privileged `about:history` page; cookie site names reach `about:settings` as JSON (`src/cookies.c:1277 json_append_string`) and are inserted with `textContent` (`src/net.c:5010-5024`); directory-listing URIs come from `g_filename_to_uri` (`src/net.c:3997-4010`). No Critical/High finding.

---

### Part A — bundled HTML / CSS

#### CONF-1 — `<html>` without `lang` on English-prose privileged pages
- Severity: **Low** · Category: a11y / spec-compliance
- Where: `src/net.c:3636` (network/HTTP error page), `src/net.c:4069` (file:// directory listing), `src/net.c:5420` (view-source), `src/net.c:5467` (view-source error), `src/net.c:5272`, `src/net.c:5289` (about:mozilla / about:book), `src/safebrowsing.c:172` (deceptive-site interstitial), `src/html.c:133`, `src/html.c:297`, `src/html.c:379` (image / JSON / XML viewers), `src/libnordstjernen.c:1314` (text viewer), `src/pdf.c:42`, `src/pdf.c:140` (PDF viewer).
```c
/* src/net.c:3636 */
g_string_append(out,
    "<!doctype html><html><head><meta charset=\"utf-8\">"
    "<title>");
```
- Spec: HTML §3.2.6.2 "The `lang` and `xml:lang` attributes" ("Authors are encouraged to specify a lang attribute on the root element"); WCAG 2.1 SC 3.1.1. The Nu validator emits "Consider adding a lang attribute to the html start tag" for each of these.
- Deviation/consequence: the start/about/settings/history/license pages correctly carry `lang="en"` but the error page, interstitial and viewers (which contain English sentences such as "Deceptive site ahead", "What to try:") do not; screen readers fall back to the UI locale, and hyphenation/`:lang()` in the engine's own UA rules pick the wrong language.
- Fix: add `lang="en"` to those twelve templates (the viewers hold almost no prose, but the error page and interstitial do; the i18n catalogue could supply the code once the strings are translated).

#### CONF-2 — logo `<img>` is both named (`alt`) and hidden (`aria-hidden`)
- Severity: **Low** · Category: a11y
- Where: `src/net.c:3024-3029`
```c
static char *
about_logo_markup(void)
{
    return g_strdup_printf("<img class=\"mark-img\" src=\"%s\" "
                           "alt=\"Nordstjernen\" aria-hidden=\"true\">",
                           about_logo_data_uri());
}
```
- Spec: HTML §4.8.4.4.1 (decorative image → `alt=""`), WAI-ARIA 1.2 §aria-hidden (hidden content must not also be the only carrier of a name). ARIA in HTML flags `aria-hidden="true"` on an element with a non-empty `alt` as a conflict.
- Consequence: the brand is already in the adjacent `<h1>`; the image should be decorative. As written, assistive tech receives contradictory signals.
- Fix: `alt=""` and drop `aria-hidden`, or keep `aria-hidden="true"` with `alt=""`.

#### CONF-3 — inline event handler in a privileged page
- Severity: **Low** · Category: security (hygiene) / spec-compliance
- Where: `src/net.c:3689-3690`
```c
g_string_append(out,
    "<button class=\"btn\" "
    "onclick=\"history.back()\">Go back</button>"
```
- Spec: HTML §8.1.7.1 event handler content attributes are valid, but CSP3 §6.1 treats them as `'unsafe-inline'`/`'unsafe-hashes'` script. Every other bundled page (`about:start`, `about:settings`, `about:history`) attaches listeners from a `<script>` block; the error page is the only one that still uses an inline handler, which prevents the error page from ever carrying a strict CSP and is the kind of pattern that becomes an injection sink if the page is later templated.
- Fix: give the button an `id` and bind `history.back()` from the page's script (or a `<script>` appended at the end of `ns_build_error_page`).

#### CONF-4 — search-engine URL is interpolated into a JS string literal with incomplete escaping
- Severity: **Low** · Category: bug
- Where: `src/net.c:5216-5223` and `src/net.c:5235-5248`
```c
GString *esc_engine = g_string_new(NULL);
for (const char *p = engine; *p; p++) {
    if (*p == '\'' || *p == '\\' || *p == '<' || *p == '"')
        g_string_append_printf(esc_engine, "\\u%04x", (guchar)*p);
    else
        g_string_append_c(esc_engine, *p);
}
...
char *with_search = about_substitute(k_about_start_template, "__ND_SEARCH_URL__", esc_engine->str);
...
char *body = about_substitute(with_name, "__ND_LOGO_MARK__", logo_markup);
```
- Spec: ECMA-262 §12.9.4 StringLiteral — `LineTerminator` (LF, CR, U+2028, U+2029) is not allowed inside a single-quoted literal; HTML §13.2.5.4 script data.
- Deviation/consequence: `<`, `"`, `'`, `\` are escaped (so there is no script injection), but a `search_engine` config value containing CR/LF/U+2028/U+2029 produces a `SyntaxError` and disables the whole start-page script. `about_substitute` (`g_strsplit`/`g_strjoinv`) replaces *all* occurrences, and the URL is substituted before `__ND_SEARCH_NAME__` / `__ND_LOGO_MARK__`, so a URL containing those literal tokens receives the (escaped) engine name or the raw `<img …>` markup inside the literal — again breaking the script. Reachable only through the user's own config/`about:settings-save`, hence Low.
- Fix: emit the value with `about_json_escape` (`src/net.c:5047`) plus `<` for `<` and ` / `, or carry it in a `data-search-url` attribute on `<form id="dsearch">` and read it with `dataset`.

#### CONF-5 — test fixtures/render tests fail Nu-validator basics
- Severity: **Low** · Category: spec-compliance
- Where: `data/render-tests/*.html` (54 files), `data/fixtures/*.html` (10 files). Counts from a scan: 0/64 have `<html lang>`; 36 render tests lack `<title>` (e.g. `data/render-tests/box.html`, `flex.html`, `table.html`, `selectors.html`); 4 lack `<meta charset>`: `data/render-tests/container-query-eval.html`, `flex-column-hypothetical-size.html`, `lists-legacy-attrs.html`, `quotes-grid-column-flow.html`.
- Spec: HTML §4.2.2 "The `title` element" (required unless a higher-level protocol provides one — `file:` does not); §4.2.5.4 (character encoding declaration required when the document is not pure ASCII and no transport charset exists). 
- Consequence: these files are the documented re-check corpus for `docs/CSS-compatibility.md`; the Nu validator reports an error per missing `<title>`/charset. Fixtures with non-ASCII text (`quotes-grid-column-flow.html` uses « » quotes) rely on default UTF-8 sniffing when opened from `file:`.
- Fix: add `<meta charset="utf-8"><title>…</title>` and `lang="en"` to each (mechanical).

#### CONF-6 — decorative inline SVGs not hidden from AT
- Severity: **Info** · Category: a11y
- Where: error page icon `src/net.c:3661-3663` (`<div class="icon">` + `ERROR_ICON_*` SVGs from `src/net.c:3376-3417`), settings nav icons `SETTINGS_NAV_ICON` `src/net.c:4785-4788`, start-page search icon `src/net.c:4625-4627`.
- Spec: WAI-ARIA 1.2 / SVG-AAM: an inline `<svg>` without `role="img"`+name or `aria-hidden="true"` is exposed as an unnamed graphic. The start-page tiles and settings nav links carry visible text so they are fine; the error-page icon and the search `<svg>` inside `<form class="search">` have no text sibling and should be `aria-hidden="true"` (the directory listing already does this correctly for its emoji icons, `src/net.c:4092`).
- Fix: add `aria-hidden="true" focusable="false"` to the `ERROR_ICON` macro and the two bare icons.

#### CONF-7 — `ns_html_escape_text` does not escape `'`
- Severity: **Info** · Category: security (defensive)
- Where: `src/html.c:91-120`
```c
case '"':
    if (escape_quotes) g_string_append(out, "&quot;");
    else               g_string_append_c(out, '"');
    break;
```
- Spec: HTML §13.1.2.3 attribute values — a single-quoted attribute value may not contain `'`.
- Status: every current attribute use is double-quoted, so this is safe today. It is a latent footgun because `g_markup_escape_text` (used by `history.c`, `safebrowsing.c`) *does* escape `'`; two escapers with different contracts in the same code base invite a future single-quoted `href='%s'`. Fix: add `case '\'': "&#39;"` when `escape_quotes` is set.

#### CONF-8 — image viewer document omits `<meta charset>`
- Severity: **Info** · Category: spec-compliance
- Where: `src/html.c:132-137`
```c
char *html = g_strdup_printf(
    "<!DOCTYPE html><html><head><title>%s</title><style>"
```
- Spec: HTML §4.2.5.4. Today this is compliant because `browser_prepare_document_response` (`src/libnordstjernen.c:1371`) overwrites the Content-Type with `text/html; charset=utf-8`, but the function is public (`ns_html_image_document`) and its `<title>` carries a URL-derived file name that may be non-ASCII; any other caller would get mojibake. Fix: add `<meta charset="utf-8">` like the JSON/XML siblings at `src/html.c:297` and `:379`.

Checked and clean: `data/org.nordstjernen.WebBrowser.metainfo.xml` and the three `docs/*.svg` badges are well-formed XML (`xmllint --noout`), declare `xmlns`, carry `<title>`/`aria-label`, and contain no scripts, external hrefs or vendor-prefixed CSS. `data/js/remote-page.js` has no HTML sink (`innerHTML` etc.); its one `outerHTML` use (`:446`) is a read. The shared `NS_ABOUT_BASE_CSS` (`src/about_style.h`) and the page styles are valid CSS: custom properties, `color-scheme`, `prefers-color-scheme`, `inline-flex`, `gap`, `:focus-within`, `::after`, `position:sticky`, `transition` are all listed ✅ in `docs/CSS-compatibility.md`; the only "vendor" items are `-apple-system`/`ui-monospace` font-family names, which are legitimate fallbacks, not prefixed properties. No duplicate `id`s were found on any page; nesting is valid (`<label>` wrapping a single control, `<nav>`/`<main>`/`<section>` sectioning, `<thead>/<tbody>` in the listing).

---

### Part B — engine parser conformance

#### CONF-9 — component-value tokenizer has no `url-token` (unquoted `url(`)
- Severity: **Medium** · Category: spec-compliance
- Where: `src/css_syntax.c:233-262` (`css_syntax_consume_ident`) and `src/css_syntax.c:308-345` (`css_syntax_parse_list`)
```c
if (!at && !hash && parser->offset < parser->length &&
    parser->input[parser->offset] == '(') {
    type = NS_CSS_COMPONENT_FUNCTION;
    parser->offset++;
}
...
if (type == NS_CSS_COMPONENT_FUNCTION) {
    component->children = css_syntax_parse_list(parser, ')');
```
- Spec: CSS Syntax 3 §4.3.4 "Consume an ident-like token" (if the name is `url` and the next code point is `(`, consume whitespace; if the next is not a quote, consume a **url token**, §4.3.6) and §4.3.14 "Consume the remnants of a bad url".
- Deviation/consequence: an unquoted `url(…)` is tokenized as an ordinary function whose body is re-tokenized. Inside it, `/*` starts a comment (`:320-323`), a `'` or `"` starts a string that runs to the end of the line (making `parser->valid = FALSE`), `(`/`)` nest, and `\` escapes are interpreted. `url(data:image/svg+xml,…'…)` or a URL with `/*` is therefore mis-parsed everywhere this tokenizer is used: `offset-path` (`src/css.c:13981`), the `<url>` type in `css_grammar.c:117-139` and `src/css_prop_syntax.c:1017-1030` (registered properties with `syntax: "<url>"`/`"<image>"`), `css_filter.c:179`, `css_anchor.c:719`, `position_component_eval` (`src/css.c:13847`). The main cascade path in `css.c` uses its own character scanner and is not affected, which is why ordinary `background:url(…)` works.
- Fix: in `css_syntax_consume_ident`, when `value` is `url` (ASCII case-insensitive) and the next non-whitespace is not `"`/`'`, consume per §4.3.6: bytes until `)`/EOF, treating `"`, `'`, `(`, non-printable and unescaped whitespace-before-non-`)` as bad-url (§4.3.14), and emit a `NS_CSS_COMPONENT_URL` (or a FUNCTION with a single STRING child) without recursing into `css_syntax_parse_list`.

#### CONF-10 — `url_serialize` truncates an unterminated `url(` at EOF
- Severity: **Low** · Category: bug
- Where: `src/css_grammar.c:117-121`
```c
char *text = component_text(c, src);
char *inner = g_strndup(text + 4, strlen(text) - 5);
```
- Spec: CSS Syntax 3 §5.4.9 "Consume a function" — on EOF "this is a parse error; return the function" (the function is valid and its `end` is EOF).
- Consequence: `css_syntax_parse_list` returns at EOF without a `)` and `component->end == length`, so `text` is `url(foo` and `strlen-5` drops the last byte of the URL (`fo`). Reachable through `style="offset-path:url(x"` or `CSS.registerProperty` initial values. Fix: strip the trailing `)` only if present (`text[len-1] == ')'`).

#### CONF-11 — `\` at EOF is not a valid escape and escapes are kept raw in token values
- Severity: **Low** · Category: spec-compliance
- Where: `src/css_syntax.c:77-83` (`css_syntax_valid_escape`), `:36-57` (`css_syntax_escape_end`), `:255-257` (`component->value = g_strndup(...)` raw), `:190-198` (string value keeps the raw escape bytes)
```c
return offset + 1 < length && input[offset] == '\\' &&
       input[offset + 1] != '\n' && input[offset + 1] != '\r' &&
       input[offset + 1] != '\f';
```
- Spec: CSS Syntax 3 §4.3.8 "Check if two code points are a valid escape" (only a newline after `\` is invalid; EOF is valid) and §4.3.7 "Consume an escaped code point" (EOF → U+FFFD; the decoded code point becomes part of the token value).
- Consequence: `-\` / `#\` / `@\` at end of input are not idents/hashes/at-keywords; more importantly `component->value` of `\61 uto` stays `\61 uto`, so `term_match` (`src/css_grammar.c:201-209`, `g_ascii_strncasecmp(c->value, term, n)`) and `syn_parse_component` ident comparisons never match escaped keywords (`@property … syntax:"auto|none"` with an escaped initial value is rejected). The cascade-side `read_css_ident` (`src/css.c:932-962`) decodes correctly, so the two tokenizers disagree on the same stylesheet text.
- Fix: decode escapes into `value` (reuse `ns_css_append_unescaped`), treat `\`+EOF as valid yielding U+FFFD.

#### CONF-12 — no §3.3 input preprocessing in `css_syntax.c`
- Severity: **Low** · Category: spec-compliance
- Where: `src/css_syntax.c:36-57`, `:172-205`, `:362-371`
```c
while (offset < length && digits < 6 && g_ascii_isxdigit(input[offset])) { offset++; digits++; }
if (offset < length && css_syntax_whitespace(input[offset])) offset++;   /* consumes only one byte */
```
- Spec: CSS Syntax 3 §3.3 "Preprocessing the input stream" (CR, FF, CRLF → LF; U+0000 and surrogates → U+FFFD), §4.3.5 "Consume a string token" (on newline: parse error, **reconsume** the newline, return bad-string).
- Consequence: `\41\r\nb` yields `A`, whitespace, `b` instead of `Ab` (the CRLF pair is one whitespace after preprocessing; `css.c:895-905` handles this correctly, the component tokenizer does not); a NUL byte becomes a DELIM instead of U+FFFD; a bad string swallows the newline, shifting what follows onto the same "line". `ns_css_syntax_is_self_contained` has the same CR/FF handling. Low because inputs arrive as UTF-8 `char*` from GLib and NUL is already impossible there.
- Fix: normalise CRLF/CR/FF and surrogate sequences in `ns_css_component_values_parse` before scanning, make the newline check in `consume_string` not advance `offset`.

#### CONF-13 — `An+B` parser strips all whitespace before matching
- Severity: **Low** · Category: spec-compliance
- Where: `src/css.c:2161-2170` (`parse_anb`)
```c
char *s = g_malloc(strlen(trimmed) + 1);
char *w = s;
for (const char *r = trimmed; *r; r++)
    if (!is_ws(*r)) *w++ = *r;
```
- Spec: CSS Syntax 3 §6 "The An+B microsyntax": `'+'? n` — "the `+` must be immediately followed by `n`"; `<n-dimension>` is a single dimension token, so `3 n` (number, ws, ident) and `2n+1 1` are invalid; whitespace is only permitted around the sign between `n` and B.
- Consequence: `:nth-child(+ n)`, `:nth-child(3 n)`, `:nth-child(2n+1 1)`, `:nth-child(2 n + 1)` are accepted (and `querySelector` does not throw) where every other engine rejects them. Harmless for valid pages; a conformance gap for `selectors/parsing/parse-nth.html`-style tests.
- Fix: tokenise the argument (`ns_css_component_values_parse` already exists) and implement the §6 grammar over tokens instead of a whitespace-stripped string.

#### CONF-14 — undeclared namespace prefix does not invalidate the style rule; `@namespace` is dropped
- Severity: **Low** · Category: spec-compliance
- Where: `src/css.c:2753-2767`, `:3014-3019` (set `g_sel_ns_prefix` + `never_match`), `:23824` (rule validity checks only `g_sel_parse_error`), `src/css.c:21077-21085` (`skip_at_rule` — `@namespace` is treated as an unknown at-rule)
```c
if (p < end && *p == '|' && !(p + 1 < end && p[1] == '=')) {
    g_sel_ns_prefix = TRUE;
    cmp->never_match = TRUE;
```
- Spec: Selectors 4 §6.1.2 "Type selectors and namespaces" ("A type selector containing a namespace prefix that has not been previously declared … is an invalid selector"), §3.9 "Invalid Selectors and Error Handling" (an invalid selector makes the whole selector list, hence the rule, invalid); CSS Namespaces 3 §3 `@namespace`.
- Consequence: `svg|rect, div { color:red }` without `@namespace svg …` must drop the whole rule; Nordstjernen keeps `div {color:red}` live. Conversely with a valid `@namespace svg url(http://www.w3.org/2000/svg); svg|rect {…}` the rule must apply but is marked `never_match`. `querySelector` is right (`ns_css_parse_selector_list_checked` at `:28293-28302` consults `g_sel_ns_prefix`), the sheet parser is not.
- Fix: parse `@namespace <prefix>? <url>` into the stylesheet, resolve prefixes against the element namespace (`NS_NODE_SVG_NS` / `data-nd-ns-uri` already exist), and treat an undeclared prefix as `g_sel_parse_error` at `:23824`.

#### CONF-15 — meta-charset prescan is a substring search; failed legacy decode re-sniffs instead of replacing
- Severity: **Medium** · Category: spec-compliance
- Where: `src/html.c:425-444` (`charset_value_in`), `src/html.c:733-830` (`ns_html_decode_body_full`), in particular `:763`, `:771-779`, `:793-806`, `:820`
```c
declared = charset_value_in(body, len < 1024 ? len : 1024);
...
} else if (!charset_is_dangerous(cs)) {
    char *out = g_convert(body, (gssize)len, "UTF-8", cs, NULL, NULL, NULL);
    if (out) { ... return out; }
}
...
if (g_utf8_validate(body, (gssize)len, NULL)) { ... return g_strndup(body, len); }
```
- Spec: HTML §13.2.3.2 "Prescan a byte stream to determine its encoding" (only `<meta>` tags are examined; comments, other tags and text are skipped; `content` is parsed with "extract a character encoding from a meta element"); §13.2.3.3 / Encoding Standard §"decode" (the chosen decoder runs with error mode *replacement* — malformed sequences become U+FFFD, the encoding is never changed mid-stream).
- Deviation/consequence: (a) `charset_value_in` matches the first `charset=` anywhere in the first 1024 bytes, including inside `<!-- -->`, a `<title>`, or a `<script>` string, so a comment such as `<!-- charset=iso-8859-5 -->` or JSON in an early script wins over the real `<meta charset>`. (b) `g_convert` with no fallback returns NULL on any malformed byte, so a `Shift_JIS`/`EUC-KR`/`GBK` page with one stray byte abandons the declared encoding and falls to `g_utf8_validate` → uchardet → windows-1252, producing whole-page mojibake where other browsers show a single U+FFFD. (c) the `windows-1252`-labelled fallback at `:820` and the hard-coded `WINDOWS-1252` alias table at `:466-494` duplicate the proper `ns_encoding_labels` table at `:452-675` which already implements the Encoding Standard label mapping but is only used by `ns_html_declared_charset`.
- Fix: route decode through `ns_html_declared_charset` (real tag-level prescan: iterate `<meta` tags, use `charset_value_in` on the `content`/`charset` attribute values only), and decode with `g_convert_with_fallback`/`GIConv` + U+FFFD replacement instead of falling through to detection when the declared encoding is known.

#### CONF-16 — scripts run after the whole document is built; `document.write` is fragment-parsed, not streamed into the input
- Severity: **Medium** · Category: spec-compliance
- Where: `src/html_lexbor.c:698-727` (`ns_html_parse_with_scripting` parses to completion), `src/js.c:72202-72257` (`ns_js_run_scripts_in_doc`), `src/js.c:69350-69378` (`ns_js_parser_hold_after` — detaches every following sibling up the ancestor chain and re-attaches them as the script "parses"), `src/js.c:65030-65083` (`ns_js_flush_document_write`), `src/js.c:65153` (`ns_written_markup_is_complete`)
```c
ns_node *fragment = ns_html_parse_fragment_with_scripting(ctx_tag, html, -1, scripting);
...
if (ref && ref->parent == parent) ns_insert_sibling_after(ref, c);
else                              ns_node_append_child(parent, c);
```
- Spec: HTML §13.2.6.4.? "in body"/`</script>` end tag handling ("… execute the script element … the parser is blocked"), §13.2.5 the **insertion point**, §8.4.3 `document.write()` step 4 ("insert input into the input stream just before the insertion point").
- Deviation/consequence: the engine emulates parser blocking after the fact (the "parser hold" is a good approximation: `document.styleSheets`, `getElementsByTagName` and `document.body.lastChild` seen by an inline script do exclude later content). But written markup is parsed as a self-contained fragment in the script's parent context and inserted after the script, so (i) `document.write('<div class=a>')` … `document.write('</div>')` across two scripts or an unclosed `<div>` that should swallow the following source produce a different tree (the fragment parser auto-closes), (ii) `document.write('<script src=…>')` followed by source that depends on that script's side effects runs in a different order than streamed parsing, (iii) `ns_written_markup_is_complete` has to guess tag balance with a text scanner. Pages built from legacy `document.write` ad/tag loaders are the ones that break. This is architectural (lexbor is run to completion, which is also what makes the parser so fast), so the practical fix is scoping, not rewriting.
- Fix: keep the hold model but (a) when a written chunk is unbalanced, re-parse `written + remaining held source` through lexbor's fragment parser in the current insertion-mode context instead of auto-closing, or at minimum (b) document the deviation in `docs/HTML-compatibility.md` §13.2.6 row (currently ✅) as 🟡.

#### CONF-17 — `<script` line/column positions are assigned by a raw text scan
- Severity: **Low** · Category: bug
- Where: `src/html_lexbor.c:644-694` (`ns_collect_script_elems`, `ns_html_assign_script_positions`)
```c
if (input[i] == '<' && i + 7 <= len &&
    g_ascii_strncasecmp(input + i, "<script", 7) == 0 &&
    (i + 7 == len || !g_ascii_isalnum(input[i + 7]))) {
```
- Spec: HTML §13.2.5 tokenizer states — `<script` inside a comment, `<textarea>`/`<title>` (RCDATA), `<xmp>`/`<noembed>`, CDATA in foreign content, or an attribute value is text, not a start tag.
- Consequence: the k-th `<script` occurrence in the bytes is paired with the k-th `<script>` element in the DOM; one `<!-- <script> -->` or a `<template>` containing a script (`ns_collect_script_elems` walks `first_child`, which excludes `tpl_content`, while the byte scan counts it) shifts every later script's `src_line`/`src_col`, so console errors point at the wrong line. Harmless for execution. Fix: have the lexbor tokenizer record the start-tag offset on the element (the fork is in-tree) instead of re-scanning.

#### CONF-18 — whitespace text node between `</head>` and `<body>` is removed
- Severity: **Low** · Category: spec-compliance
- Where: `src/html_lexbor.c:617-642` (`ns_prune_html_interelement_whitespace`), called from `:724`
```c
if (c->kind == NS_NODE_TEXT && ns_text_all_whitespace(c->text)) {
    ns_node_remove(c);
    ns_node_free(c);
}
```
- Spec: HTML §13.2.6.4.6 "The 'after head' insertion mode" — "A character token that is one of U+0009 … U+0020: **Insert the character**" (into the `html` element).
- Consequence: lexbor builds the spec tree (`html` → `head`, `#text("\n")`, `body`); the pruning turns `document.documentElement.childNodes.length` from 3 into 2 and changes `head.nextSibling`, which `dom/nodes` tests and some libraries (`node.nextSibling === document.body` checks) observe. Fix: drop the pruning and have layout ignore inter-element whitespace at the `html` level instead (it already does for `head`).

#### CONF-19 — DOCTYPE with a missing name is reported as `html`
- Severity: **Low** · Category: spec-compliance
- Where: `src/html_lexbor.c:111-124`
```c
ns_node_set_name_borrow(out, name && nlen ? (const char *)name : "html");
```
- Spec: HTML §13.2.6.4.1 "The 'initial' insertion mode" — "Append a DocumentType node … whose name is the name given in the DOCTYPE token, **or the empty string if the name was missing**".
- Consequence: `<!DOCTYPE>` (quirks) serialises/reports as `<!DOCTYPE html>` and `document.doctype.name === ""` fails. Fix: use `""` when `nlen == 0`.

#### CONF-20 — `ns_css_node_dir` ignores slots and shadow hosts
- Severity: **Low** · Category: spec-compliance (also the root of a WPT cluster, see below)
- Where: `src/css.c:25610-25632` (`ns_css_node_dir`), `src/css.c:25492` (`ns_dir_auto_resolve`)
```c
for (const ns_node *n = el; n; n = n->parent) {
    if (n->kind != NS_NODE_ELEMENT) continue;
    const char *dir = ns_element_get_attr(n, "dir");
```
- Spec: HTML §3.2.6.4 "The directionality" — a `dir=auto` `<slot>` takes its direction from its assigned nodes; a `<slot>` without `dir` inherits from its shadow **host**, not its flat-tree parent; `dir=auto` text collection skips `<script>`, `<style>`, `<textarea>`/`<input>` values, and nodes with their own `dir`.
- Consequence: 18 failing subtests in `html/dom/elements/global-attributes/dir-auto-dynamic-changes.window.html` and `dir-slots-directionality.html` (see cluster 2). Fix: walk the flat tree (`css_shadow_root_of` / `css_find_slot` at `:25504-25520` already exist) when resolving `dir`, and for a `<slot>` consult `assignedNodes` in `ns_dir_auto_resolve`.

---

### Largest failing WPT clusters → code

The tracked slice (`docs/wpt-scores.md` ROI table) contains no `css/` directory, so the CSS parser rows above cannot be cross-checked against `docs/wpt-subtests.tsv`; the HTML/DOM clusters below are the ones that belong to this review's scope. `WebCryptoAPI/digest` (371), `url/*` (177) and `dom/ranges/tentative/OpaqueRange-*` (~170) are already classified as non-goals or separate work in "Top 10 improvements".

| # | Cluster (non-passing subtests) | Representative subtest names | Responsible code | Spec |
|---|---|---|---|---|
| 1 | `html/dom/elements/the-innertext-and-outertext-properties/getter.html` — **30 FAIL** | "Tab-separated table cells", "Single newline in two-row inline-table", "Whitespace around inline-block should not be collapsed", "`<canvas>` should not collapse following space", "`display:block <script>` is rendered", "`<optgroup>` containing `<option>`", "unopened `<details>` ignored", "Shadow DOM contents ignored", "::first-line styles applied", "text-transform handles Turkish casing" | `ns_element_get_innerText` `src/js.c:7568-7592` → `ns_inner_text_collect` (same file; starts the walk with `NS_IT_WS_NORMAL`). Missing: `display:table-cell` → TAB and `table-row` → LF steps, atomic-inline (replaced element / `inline-block`) whitespace boundaries, `display:block` on `script`/`style`, `::first-line`/`::first-letter` `text-transform`, `details` closed-content skip, shadow-tree exclusion, `lang`-aware case mapping | HTML §3.2.7 "inner text collection steps" (steps 4–8), CSS Text 3 §4.1 |
| 2 | `global-attributes/dir-auto-dynamic-changes.window.html` (11) + `dir-slots-directionality.html` (7) — **18 FAIL** | "slot provides updated directionality from host to a dir=auto container", "Removing slotted content resets direction on dir=auto slot", "dir=auto slot ignores text in value of input element children", "dynamic changes to subtrees excluded as a result of the dir attribute" | `ns_css_node_dir` `src/css.c:25610`, `ns_dir_auto_resolve` `src/css.c:25492` (CONF-20); restyle invalidation on slot reassignment lives in the incremental-style code (`incr_*` in `src/css.c:34598+`) which does not key on `dir` of slotted content | HTML §3.2.6.4 |
| 3 | `dom/nodes/attributes.html` — **17 FAIL** | "Basic functionality of getAttributeNode/getAttributeNodeNS", "setAttributeNode", "removeAttributeNode", "Attribute loses its owner when removed", "Own property names should only include all-lowercase qualified names…", "Replacing an attr by itself" | `ns_element_getAttributeNode` `src/js.c:38988`, `ns_element_getAttributeNodeNS` `:39013`, `ns_element_setAttributeNode` `:39052` (and the `setAttributeNodeNS` alias at `:55668`). `Attr` objects are created per call rather than being stable wrappers with `ownerElement`, and `NamedNodeMap`'s supported property names are not filtered per the §"attribute list" rules | DOM §4.9.1 `Attr`, §4.9.2 `NamedNodeMap` ("supported property names": lowercase-qualified-name filter for HTML elements in HTML documents) |
| 4 | `dom/nodes/CharacterData-surrogates.html` (8) + `Node-textContent.html` "d\0e" (8) — **16 FAIL** | "Text.substringData() splitting surrogate pairs", "Comment.insertData() splitting and creating surrogate pairs", "Element with children set to \"d\\0e\"" | `ns_element_substring_data` `src/js.c:7710-7731` and the sibling `insertData`/`deleteData`/`replaceData` map UTF-16 offsets onto UTF-8 with `ns_utf16_offset_to_ptr`; a lone surrogate produced by splitting a pair cannot be stored in UTF-8 `n->text`, so it is dropped/merged. The NUL case keeps `text_len` on set (`ns_element_set_textContent` `src/js.c:8784-8809` uses `JS_ToCStringLen` + `ns_node_new_text_len`) — the failing assertion is on a later read path that uses a NUL-terminated string; exact getter **unconfirmed** (candidates: `ns_node_collect_all_text_len` callers, `JS_NewString(ctx, c.out->str)` in innerText) | DOM §4.10 `CharacterData` (UTF-16 code-unit semantics), Infra §4.6 |

---

### What is done well

- **Privileged pages are escape-complete.** Every interpolation of a URL, hostname, file name, error string or title in `net.c`, `history.c`, `safebrowsing.c`, `html.c`, `pdf.c`, `libnordstjernen.c` is routed through `ns_html_escape_text`/`g_markup_escape_text`; the settings page builds DOM with `createElement`/`textContent` from JSON instead of `innerHTML`; history only stores `http(s)` URLs; `about:settings*`/`about:history` are gated on the top-level URL being chrome (`about_request_from_chrome`, `src/net.c:5168-5181`).
- **`<template>` is honoured end to end**: `lxb_walk_into` (`src/html_lexbor.c:160-195`) moves lexbor's template content into `ns_template_content_get(...)`, and both the selector engine and `ns_js_collect_script_tasks_rec` (`src/js.c:68931-68949`) stop at templates, so template scripts never run (HTML §4.12.3).
- **Selectors 4 edge rules are implemented carefully**: `:where()` contributes zero specificity (`src/css.c:2935-2952`), `:has()` inside `:has()` is rejected (`:2979-2983`), pseudo-elements inside logical combinators are rejected (`:2794`), the pseudo-element/pseudo-class compatibility tables (`css_pseudo_element_may_follow`, `css_pseudo_class_may_follow`) encode CSS Pseudo-Elements 4 §4, and `ns_css_html_ci_attr` (`:25898-25913`) matches the HTML "case-sensitivity of selectors" attribute list exactly.
- **CDO/CDC are only skipped at the top level** (`!nested`, `src/css.c:23265-23271`) exactly as CSS Syntax 3 §5.4.1 prescribes, and the cascade-side escape decoder handles CRLF after a hex escape and maps 0/surrogates/out-of-range to U+FFFD (`css_unescape_cp`, `css_append_hex_escape`, `src/css.c:881-907`).
- **Denial-of-service guards are present in every recursive parser**: `NS_CSS_SYNTAX_MAX_NESTING 128`, `NS_CSS_MAX_SELECTOR_NESTING 48`, `NS_CSS_MAX_AT_NESTING 32`, depth caps of 512 in the DOM walkers, and a 1 MiB cap on the uchardet scan.
- **Charset label table** (`ns_encoding_labels`, `src/html.c:452-675`) is a faithful copy of the Encoding Standard's label list including the `replacement` and `x-user-defined` special cases; the fix for CONF-15 is mostly a matter of using it.

### Count by severity

| Severity | Count | IDs |
|---|---|---|
| Critical | 0 | — |
| High | 0 | — |
| Medium | 3 | CONF-9, CONF-15, CONF-16 |
| Low | 14 | CONF-1, CONF-2, CONF-3, CONF-4, CONF-5, CONF-10, CONF-11, CONF-12, CONF-13, CONF-14, CONF-17, CONF-18, CONF-19, CONF-20 |
| Info | 3 | CONF-6, CONF-7, CONF-8 |

Total: 20 findings (3 Medium, 14 Low, 3 Info) — no security-grade issue in the bundled pages.

---

## Security review — vendored engine forks in Nordstjernen (HEAD a58ff38)

Static review only (no build possible: GTK missing). Scope: the **local
modifications** to `src/quickjs/` (quickjs-ng fork), `src/lexbor/` (lexbor
fork), `src/web-text-lib/` (Pango fork), `subprojects/` wraps + download
scripts, and `src/wasm.c` + `src/wamr/`.

Method: upstream quickjs-ng `1b519fa` and lexbor `145872a` (the documented
fork points) were fetched and diffed file-by-file against the fork, so every
"local change" below is a verified delta, not a guess. The quickjs.c delta is
416 hunks / ~6.4k changed lines; the lexbor delta is confined to `url/url.c`,
`unicode/idna.{c,h}`, `encoding/decode.c` (decoder trimming), `core/mraw.h`.
Every finding was confirmed by reading the code; items that could not be
confirmed are marked **unconfirmed** with the reason.

Two premise corrections first:

- **There is no JIT in the QuickJS fork.** `grep -i 'jit|mprotect|PROT_EXEC|VirtualAlloc|mmap('` over `src/quickjs/*.{c,h}` returns nothing; the interpreter is pure bytecode. The W^X / JIT questions therefore do not apply to the engine (see VEN-3 for the seccomp side and VEN-9 for the doc that claims a JIT exists).
- **`src/wasm.c` is not an in-house interpreter.** It is the JS-API binding over the vendored WAMR 2.4.5 fast interpreter in `src/wamr/` (`docs/webassembly.md`). Opcode validation, branch arity, `call_indirect` signature checks, div-by-zero / float→int traps and segment bounds are WAMR's loader/interpreter (upstream code, `WASM_DISABLE_HW_BOUND_CHECK=1`, software bounds checks). Those were not re-audited as "local modifications"; the binding, the local WAMR patches (`ns_wamr.c`) and the memory/table glue were.

---

### Findings

#### VEN-1 — Worker runtimes never install the CSP string-compile hook (eval bypass in workers)
- **Severity:** Medium  **Category:** security
- **Where:** `src/js.c:30117-30120` (worker runtime setup) vs `src/js.c:62925-62930` (main runtime); CSP headers for the worker are parsed at `src/js.c:30090-30091`.

```c
    JS_SetInterruptHandler(js->rt, ns_js_interrupt_cb, js);
    JS_SetMemoryLimit(js->rt, (size_t)mb * 1024 * 1024);
    JS_SetHostPromiseRejectionTracker(js->rt, ns_worker_promise_rejection_tracker, NULL);
    JS_SetMaxStackSize(js->rt, ns_worker_stack_limit(host));
    /* no JS_SetCanCompileStrings() here — the main realm sets it at 62929 */
```

- **Trigger:** A page (or worker script response) carries `Content-Security-Policy: script-src 'self'` (no `'unsafe-eval'`). Inside a `Worker`/`SharedWorker`, `eval("...")` or `new Function("...")` is executed.
- **Consequence:** The fork's `rt->can_compile_strings` hook is the only thing that makes `eval`/`Function` ask the browser (`docs/quickjs.md`, "CSP and eval"). The worker runtime copies the CSP headers into `js->csp` but never calls `JS_SetCanCompileStrings`, so `ns_js_string_compile_allowed()` is never consulted and the `unsafe-eval` restriction is silently not enforced in workers. (String timers and WebAssembly compile are checked by the browser itself and are unaffected: `ns_wasm_module_from_bytes` calls `ns_wasm_compile_allowed` at `src/wasm.c:1835`.)
- **Fix:** Add `JS_SetCanCompileStrings(js->rt, ns_js_can_compile_strings);` next to the other `JS_Set*` calls in the worker runtime constructor. `ns_js_realm_csp()` already returns `js->csp` for the worker's main realm, so nothing else is needed.

#### VEN-2 — `Atomics.waitAsync` promises never settle; waiters accumulate until the runtime dies
- **Severity:** Low  **Category:** bug
- **Where:** `src/quickjs/quickjs.c:67430` (`js_atomics_wait_async`), `:67502` (`js_atomics_settle_async_waiters`), `:67582` (`JS_RunAtomicsWaiters`); no caller anywhere under `src/` outside `src/quickjs/` (`grep -rn 'JS_RunAtomicsWaiters|JS_HasAtomicsWaiters' src --include=*.c` → empty).

```c
    waiter->ctx = JS_DupContext(ctx);
    waiter->resolve = funcs[0];
    js_mutex_lock(&js_atomics_mutex);
    waiter->linked = true;
    list_add_tail(&waiter->link, &js_atomics_waiter_list);
    list_add_tail(&waiter->async_link, &ctx->rt->atomics_async_waiters);
```

- **Trigger:** `Atomics.waitAsync(new Int32Array(new SharedArrayBuffer(4)), 0, 0, 10).value.then(...)` on a page.
- **Consequence:** The fork added waitAsync to the engine with an explicit embedder pump (`JS_RunAtomicsWaiters`), but `src/js.c` never calls it, so timed-out or notified waiters are never settled: the promise hangs forever and every call leaks a `JSAtomicsWaiter` + a dup'ed `JSContext` until `JS_FreeRuntime`. Growth is bounded by `JS_SetMemoryLimit` (waiters are `js_mallocz`), so this is a functional bug and a minor memory sink, not a crash. Memory-safety of the implementation itself looks right: `is_waitable == 2` rejects non-shared buffers in `js_atomics_get_buf` (SABs cannot shrink or detach, so the raw `ptr` stays valid), and `js_atomics_free_async_waiters` runs at runtime teardown.
- **Fix:** In the renderer's microtask/event pump, call `JS_RunAtomicsWaiters(rt, false)` whenever `JS_HasAtomicsWaiters(rt)` (and schedule a wake-up at the next deadline), or drop waitAsync from the global object until the pump exists.

#### VEN-3 — Executable-memory syscalls stay allowed in the default renderer (WebGL/WebGPU on)
- **Severity:** Low  **Category:** security (defence-in-depth)
- **Where:** `src/libnordstjernen.c:1181-1200`; rules in `src/security.c:966-984`.

```c
    gboolean gpu_apis = ns_config_get()->webgl_enabled ||
                        ns_config_get()->webgpu_enabled;
    ...
    ns_security_seccomp_deny_unix_sockets();
    if (!gpu_apis) {
        ns_mic_preload();
        ns_security_seccomp_deny_exec_memory();
    }
    ns_security_seccomp_init();
```

- **Trigger:** Default configuration (`webgl_enabled` and `webgpu_enabled` are on by default per `CLAUDE.md`).
- **Consequence:** `mmap`/`mprotect`/`pkey_mprotect` with `PROT_EXEC` are only denied when both GPU APIs are off, because GPU drivers need JIT-able memory. The engine itself never maps executable memory (no JIT, see above), so this is not an engine bug, but `docs/Software-Architecture.md:52-55` ("presents no W^X surface") describes a guarantee the syscall filter does not enforce for the common configuration: a renderer compromise can still create RWX pages. This is an accepted trade-off, recorded here so the documentation and threat model match.
- **Fix:** Document the condition in `docs/Software-Architecture.md` / `docs/tab-isolation.md`; longer term, consider denying `mprotect(PROT_EXEC)` on anonymous mappings only (allow file-backed `PROT_EXEC` for driver shader blobs) or moving GPU work out of the renderer.

#### VEN-4 — WebAssembly linear memory growth from an instance is not capped by the browser
- **Severity:** Low  **Category:** security (DoS)
- **Where:** `src/wasm.c:766-776` (`ns_wasm_memory_grow`, instance path) and wasm-side `memory.grow`; the 512 MiB cap `NS_WASM_MEMORY_MAX_BYTES` (`src/wasm.c:31`) is applied only to the staging path (`:747`) and the `Memory` constructor (`:2195`).

```c
    wasm_memory_inst_t mem = wasm_runtime_get_memory(wi->inst, 0);
    ...
    uint32_t old_pages = (uint32_t)wasm_memory_get_cur_page_count(mem);
    if (delta) {
        if (!wasm_memory_enlarge(mem, delta))
            return JS_ThrowRangeError(ctx, "wasm memory.grow failed");
        ns_wasm_memory_detach_buffer(m);
    }
```

- **Trigger:** A module declaring a memory with no maximum (or `maximum: 65536`) grows it from wasm or via `exports.memory.grow()` in a loop.
- **Consequence:** WAMR uses the system allocator (`Alloc_With_System_Allocator`, `src/wasm.c:187`) and allocates outside the QuickJS `JS_SetMemoryLimit`, so a page can commit up to 4 GiB per instance, repeated per instance, until the renderer is OOM-killed. Containment: per-tab renderer process; the shell restarts it. The 512 MiB cap already exists for JS-created memories, so the inconsistency looks accidental.
- **Fix:** Check `(old_pages + delta) * 64 KiB <= NS_WASM_MEMORY_MAX_BYTES` before `wasm_memory_enlarge`, and clamp the module's declared max pages at load (`wasm_runtime_load_ex` → inspect `max_memory_pages`, or reject modules whose max exceeds the cap) so wasm-side `memory.grow` is capped too.

#### VEN-5 — Regexp capture limit raised 255 → 65535 makes the search prologue O(len × captures)
- **Severity:** Low  **Category:** security (DoS, bounded by the eval budget)
- **Where:** `src/quickjs/libregexp.c:71` (`CAPTURE_COUNT_MAX 65535`), `:3745` and `:3860` (`re_exec_search` / sticky search).

```c
    for(;;) {
        ...
        memset(capture, 0, sizeof(*capture) * s->capture_count * 2);
        ret = lre_exec_backtrack(s, capture, pc, cptr);
        if (ret != 0)
            return ret;
        if (pf->kind == RE_PREFILTER_ANCHORED || cptr >= cbuf_end)
            return 0;
        if (lre_poll_timeout(s))
            return LRE_RET_TIMEOUT;
```

- **Trigger:** A pattern with tens of thousands of groups (`"()".repeat(60000)` + a non-matching tail) executed against a multi-megabyte string.
- **Consequence:** The fork's new C search loop clears all `2 × capture_count` pointers (≈1 MiB) at every candidate position, so a non-matching search costs ~1 MiB of `memset` per character. The interrupt handler is polled (`lre_poll_timeout` → `lre_check_timeout` → `ns_js_interrupt_cb`), so the damage is a single tab hung for the eval budget (`js_eval_budget_ms`, default 60 s) rather than a crash; memory is fine (captures are `js_malloc`'d via `alloc_count` at `quickjs.c:53633`, `:54328`, `:54660`, no `alloca`). The 16-bit header/opcode widening itself is consistent across emit, dump, validate and exec.
- **Fix:** Either keep `CAPTURE_COUNT_MAX` at 255 (web engines' effective limits are far lower than 65535 in practice) or make `re_exec_search` reset only the captures the body touches (as the `REOP_save_reset` bytecode already does), e.g. by resetting after a failed attempt only the range `[0, capture_count)` that was written.

#### VEN-6 — Proper tail calls turn accidental infinite recursion into a 60-second hang
- **Severity:** Low  **Category:** bug (web compatibility / DoS)
- **Where:** `src/quickjs/quickjs.c:19068` (`js_tail_call_reuses_frame`), `:19650` (`tail_call:` frame reuse in `JS_CallInternal`), `:39054` (`js_code_reaches_return` in `resolve_labels`).

```c
    if (!b->is_strict_mode || b->func_kind != JS_FUNC_NORMAL ||
        sf->is_constructor || JS_VALUE_GET_TAG(func_obj) != JS_TAG_OBJECT)
        return false;
    p = JS_VALUE_GET_OBJ(func_obj);
    return p->class_id == JS_CLASS_BYTECODE_FUNCTION &&
        p->u.func.function_bytecode->func_kind == JS_FUNC_NORMAL;
```

- **Trigger:** Any strict-mode (i.e. every bundled ES module) pair `function f(){ return g() } function g(){ return f() }` reached by accident.
- **Consequence:** Every shipping browser except Safari throws `RangeError: Maximum call stack size exceeded` within milliseconds; here the frame is reused, `js_poll_interrupts` keeps it running, and the page spins until `ns_js_interrupt_cb` halts it after the eval budget. Memory safety of the reuse was checked and looks correct: `close_var_refs` runs before the operand stack is freed, the freed region stops at the callee/this slots which are moved (not dup'ed) into `dst[0..1]` and freed at frame exit with the rest of `local_buf`, `alloca` only grows monotonically and is guarded by `js_check_stack_overflow`, and the compiler only emits the reused form when the call is immediately followed by `return` or by the `nip_catch; gosub` exit of a catch clause without `finally` (`empty_catch_finally`, set at `:32504`), which is a tail position per spec.
- **Fix:** Consider a per-frame tail-call counter (e.g. bail to a normal call after N reused frames so the stack limit still fires), or gate frame reuse behind a config flag. At minimum document the behaviour difference versus V8/Gecko.

#### VEN-7 — Legacy `fn.caller` / `fn.arguments` resolve live frames without a realm check
- **Severity:** Low  **Category:** security (information disclosure, cross-realm)
- **Where:** `src/quickjs/quickjs.c:9849-9864` (hook in `JS_GetPropertyInternal`), `:17434` (`js_function_caller_get`), `:17420` (`js_function_arguments_get`).

```c
    for (JSStackFrame *sf = ctx->rt->current_stack_frame; sf; sf = sf->prev_frame) {
        if (JS_VALUE_GET_OBJ(sf->cur_func) != target)
            continue;
        JSStackFrame *caller = sf->prev_frame;
        ...
        if (!caller || !JS_IsObject(caller->cur_func) || !cb ||
            cb->is_strict_mode || cb->func_kind != JS_FUNC_NORMAL ||
            cb->func_name == JS_ATOM__eval_)
            return JS_NULL;
        return js_dup(caller->cur_func);
```

- **Trigger:** Realm A holds a reference to a sloppy function `f` (e.g. a callback it handed to a same-origin frame B). While B's code calls `f`, A (re-entered through a getter or a nested call) reads `f.caller` / `f.arguments`.
- **Consequence:** The getter returns B's calling function object and the live argument values regardless of which realm they belong to. V8 implements the same legacy behaviour but refuses when the caller's context does not share the security token, and also returns `null` for class constructors/generators. In Nordstjernen cross-origin frames already cannot obtain each other's functions through the WindowProxy, so exploitability is limited to same-origin realms; the gap is defence in depth. Strict callers and `eval` frames are correctly hidden; C-function frames break the walk (returns `null`), which is conservative.
- **Fix:** Return `JS_NULL` when `cb->realm != b->realm` (or when the two realms' documents are not same-origin, via an embedder callback), and when `cb->is_class_constructor`.

#### VEN-8 — `Object.freeze/seal` on a length-tracking typed array no longer rejected
- **Severity:** Low  **Category:** bug (spec regression, not memory-unsafe)
- **Where:** `src/quickjs/quickjs.c:45412` (`js_object_seal`); the fork deleted upstream's block:

```c
-    if (freeze_flag && is_typed_array(p->class_id)) {
-        ta = p->u.typed_array;
-        abuf = ta->buffer->u.array_buffer;
-        if (array_buffer_is_resizable(abuf) || typed_array_is_oob(p))
-            return JS_ThrowTypeError(ctx, "cannot freeze resizable typed array");
-    }
```

- **Trigger:** `Object.freeze(new Uint8Array(new ArrayBuffer(0, {maxByteLength: 16})))` then `rab.resize(16)`.
- **Consequence:** ES2024 TypedArray `[[PreventExtensions]]` returns false for non-fixed-length views, so freeze/seal/preventExtensions must throw. With the check removed an empty length-tracking view freezes successfully and later gains writable elements while reporting `Object.isFrozen() === true`. Element access still goes through `p->u.array.count`, so there is no out-of-bounds consequence. No `IsTypedArrayFixedLength` equivalent exists elsewhere in the fork (`grep 'cannot freeze|fixed_length'` → nothing).
- **Fix:** Restore the check (ideally in `JS_PreventExtensions` so `Object.preventExtensions`/`seal` are covered too), unless a WPT/test262 case motivated the removal — in which case record it in `docs/vendored-engine-updates.md`.

#### VEN-9 — Documentation describes a JIT that does not exist and a wrap pin that moved
- **Severity:** Info  **Category:** bug (documentation)
- **Where:** `CLAUDE.md:166` ("we modify it freely — JIT, profiling, and other browser-side hooks live there"); `docs/quickjs.md:30` says the wrap pins `19d6bd8` while `subprojects/quickjs.wrap:3` pins `712e8c0ca26949477c38f91dbaf955cc48e2f261` (and `docs/quickjs.md:19` already says `712e8c0`).
- **Consequence:** Reviewers (and the task brief) start from a wrong threat model (W^X, code-generation bugs). `docs/Software-Architecture.md:52-55,363` is correct that there is no JIT.
- **Fix:** Reword `CLAUDE.md` to "browser-side hooks and compatibility changes"; make `docs/quickjs.md:30` match the wrap.

#### VEN-10 — The QuickJS fork is compiled with all warnings disabled
- **Severity:** Info  **Category:** bug (process)
- **Where:** `src/quickjs/meson.build:8` (`qjs_warning_args = ['-w']`).
- **Consequence:** `CLAUDE.md`'s "compiles cleanly (no new warnings)" gate does not see the ~6.4k locally modified lines in `quickjs.c`/`libregexp.c`, where most of the browser-specific logic lives. Lexbor and web-text-lib are not built with `-w`.
- **Fix:** Drop `-w` and suppress only the specific upstream warning families (`-Wno-unused-parameter`, …), so new local code gets diagnostics.

#### VEN-11 — Main-realm JS memory cap from config is not clamped
- **Severity:** Info  **Category:** bug (hardening)
- **Where:** `src/js.c:62922-62926` (`mb` from `js_memory_cap_mb`, default 2048, `if (mb <= 0) mb = 2048;` only) vs the worker path `src/js.c:30113-30118` which clamps to 512.
- **Consequence:** A misconfigured value like `js_memory_cap_mb = 100000` is used as-is (`(size_t)mb * 1024 * 1024`); on 32-bit builds this overflows `size_t`. Low impact (local config file).
- **Fix:** Clamp to a sane maximum as the worker path does.

#### VEN-12 — Vendored WAMR 2.4.5: upstream vulnerability tracking (unconfirmed)
- **Severity:** Info  **Category:** supply-chain
- **Where:** `src/wamr/core/version.h:19-21` (2.4.5); local patches listed in `docs/webassembly.md`; `ns_wamr.c` accessors bounds-check `idx >= table->cur_size` and funcref writes from JS are refused (`src/wasm.c:913-919, 952-956`), so the narrow accessors do not widen the attack surface.
- **Consequence:** WAMR's loader has a history of memory-safety CVEs; a vendored copy only gets fixes when `docs/vendored-engine-updates.md`-style refreshes happen. **Unconfirmed**: I could not check the CVE feed offline for issues fixed after 2.4.5.
- **Fix:** Add WAMR to the refresh log cadence and record the upstream commit in `docs/webassembly.md`.

#### VEN-13 — `wrap-git` pin for the original QuickJS relies on a SHA-1 commit id only
- **Severity:** Info  **Category:** supply-chain
- **Where:** `subprojects/quickjs.wrap` (`[wrap-git]`, full 40-hex `revision`, `depth = 1`, no `source_hash`, which wrap-git does not support).
- **Consequence:** Content identity rests on GitHub serving the right object for the SHA-1; acceptable, and this build is opt-in (`-Dquickjs=quickjs`) and not run in CI.
- **Fix:** Optional: switch to a `[wrap-file]` tarball of the pinned commit with a `source_hash`.

---

### Reviewed and found sound (no finding)

- **Runtime limits are set.** Main realm: interrupt handler, `JS_SetMemoryLimit` (default 2 GiB), `JS_SetCanCompileStrings`, `JS_SetMaxStackSize(5 MiB)` (`src/js.c:62925-62930`); workers: interrupt, memory (clamped 512 MiB), stack = ¾ of the thread stack (`:30117-30120`). `ns_js_interrupt_cb` (`:724`) enforces an eval budget and a 60 s monitor; `lre_check_timeout` routes regexp execution through it.
- **Typed-array sort** (`js_typed_array_sort`, `quickjs.c:~66030`): the fork sorts indices against a **snapshot copy** of the elements, so a comparator that detaches/shrinks the buffer cannot make the comparator read freed memory; write-back re-checks `typed_array_is_oob` and clamps to the live count.
- **`js_typed_array_constructor`** re-ordering: `js_create_from_ctor` now runs before `ToIndex(offset/length)`; `abuf->detached` and byte_length are read after the last user-code call and immediately before `typed_array_init`.
- **Atomics**: `js_atomics_revalidate` re-checks OOB and index after operand coercion in `op`/`store`; `notify` only derefs when `abuf->shared`.
- **`js_proxy_get_own_property`**: keeps the target descriptor's values alive until `done:` and frees on every exit path, including the early `goto fail`.
- **`js_set_difference/union/symmetricDifference`**: the clone built by `js_set_clone_records` is never exposed to user code during iteration.
- **`JS_RepointArrayBuffer`** refuses detached or engine-owned (`realloc_func != NULL`) buffers; `src/wasm.c` detaches the `Memory.buffer` on every grow (JS-side and via the WAMR enlarge-success callback), guards `ArrayBuffer.prototype.transfer*` against memory buffers, and `ns_wasm_instance_finalizer` detaches before `wasm_runtime_deinstantiate`.
- **Bytecode reader**: `JS_READ_OBJ_BYTECODE` is only fed from the per-process in-memory cache (`src/bytecode_cache.c`) and the engine's own bundles; persisted IndexedDB data is read without `allow_bytecode`.
- **Engine-private names / host access** (`js_access_atom`, `JS_IsHostAccess`, `js_hide_engine_global_keys`): builtins are transparent ("act for whoever called"), only embedder C functions and hidden-source engine code count as host, and `JS_SetPrototypeInternal` honours `is_immutable_prototype` for page code only.
- **Legacy RegExp statics**: positions are stored alongside the subject string they index; `regexp_legacy_pos[20]` is written for at most 10 groups; invalidated on subclass/cross-realm matches.
- **Lexbor fork**: `lexbor_mraw_data_size` uses `memcpy` (alignment fix); `url.c` drops non-UTF-8 output encodings (correct for a UTF-8-only engine); the local IDNA layer (`unicode/idna.c`) implements CheckBidi/CheckJoiners with every index guarded (`len >= 4` before `cps[3]`, `len > 0` before `cps[len-1]`, binary-search property table with a 0 default); `src/html_lexbor.c` includes only public headers; URL serialization in `src/net.c` is callback-based into a `GString`, so there is no fixed output buffer to overflow (the IPv4/IPv6 host serializers write into `LXB_URL_BUFFER_NUM_SIZE` with bounded formatters — upstream code).
- **web-text-lib shape cache** (`src/web-text-lib/ns-shape-cache.c`): the key covers font (strong-ref'd, so no pointer reuse while cached), language, level, gravity, script, analysis/shape/show flags, text transform, hyphen kind, clipped feature ranges and the text bytes, with a full `key_equal` behind the hash; runs are only cached when both item edges are context-independent (`context_independent`, `boundary_char`), and words are split only at space boundaries. Bounded by 20 000 entries / 48 MiB with a read-bit sweep, 16 sharded `GRWLock`s, dropped on font-map change. The break cache stores only language-independent `default_break` output (`break.c:2503-2509`); the item cache keys on the context serial, base direction, text and attribute identity. `NS_PANGO_SHAPE_CACHE=0|off|verify` is read once, atomically. Layout is single-threaded per renderer (`docs/threading.md`), so the locks are belt-and-braces.
- **Supply chain**: every `[wrap-file]` (`simde`, five `wgpu-native-*`) carries a `source_hash`; `scripts/fetch-wgpu-native.sh` takes its checksum from the wrap and fails without one; `scripts/build-ffmpeg-lgpl.sh` pins SHA-256 for the default FFmpeg versions and refuses other versions without `FFMPEG_SHA256`; `scripts/pack-appimage.sh` pins all four tool downloads. Wuffs and pl_mpeg are vendored sources, not downloads.

### What is done well

- The vendored forks are kept honest: `docs/vendored-engine-updates.md` records the fork base, the three-way merge classification per file and every preserved local hook per refresh, which made this diff-based review possible.
- Typed-array hot spots were hardened *beyond* upstream (snapshot sort, re-validation in Atomics, constructor re-ordering) rather than patched ad hoc.
- The CSP `eval` hook and the WebAssembly compile gate are enforced inside the engine/binding, not in page-visible JS.
- WebAssembly runs with hardware bound checks disabled in favour of software checks, no WASI, no sockets, and `Memory.buffer` is detached on every move, including wasm-initiated grows.
- The shape cache is keyed on every shaping input, strong-refs fonts, bounds its memory, and ships a `verify` mode that re-shapes to catch key omissions.
- All third-party downloads are checksum-pinned; nothing unpinned was found.

### Count by severity

| Severity | Count | IDs |
|---|---|---|
| Critical | 0 | — |
| High | 0 | — |
| Medium | 1 | VEN-1 |
| Low | 7 | VEN-2, VEN-3, VEN-4, VEN-5, VEN-6, VEN-7, VEN-8 |
| Info | 5 | VEN-9, VEN-10, VEN-11, VEN-12 (unconfirmed CVE status), VEN-13 |

---

