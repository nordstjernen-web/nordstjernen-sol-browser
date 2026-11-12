# Code review report

Review of Nordstjernen at commit `6ac1c8c` (1.0.47-dev, 2026-11-12): unused
code, duplicate code, insecure code, code a library could replace, libraries
that would harden the browser, and dependencies from the Mozilla project.
The review itself changed no source file, and every recommendation is a
proposal. The one exception is an engine bug found while trying PDF.js
(section 11), which is fixed.

## 1. Summary

### What the review found

- **No memory-corruption bug was confirmed** in the first-party code that was
  read. The in-tree parsers (MP4, WOFF2, ICO), the IPC framing, canvas
  `putImageData`, WebGL transfer sizing and WebCrypto AES are bounds- and
  overflow-checked throughout.
- **The weak point is the trust boundary, not the parsers.** The shell
  believes things a renderer tells it, and a renderer can reach more than
  `SECURITY.md` says. Six findings (S1–S6) all need an attacker who already
  runs code in a renderer, which is exactly the attacker the process split
  exists to contain.
- **Dead code is rare.** The warning-clean build has already removed every
  unused static and macro. What remains is about 75 lines of dead exports,
  seven config keys nothing reads, and 111 functions with wider linkage than
  they need.
- **Duplication is re-implementation, not copy-paste** (1.2% exact clones).
  It is concentrated in CSS value scanning, `calc()` evaluation, SVG path
  parsing and JSON handling.
- **Almost nothing should be replaced by a new library.** The wins use code
  already linked or already in the tree. expat for `src/xml.c` was the one
  external candidate; on closer study (section 10) it buys conformance, not
  safety.
- **Build hardening is good on Linux x86-64 and thinner elsewhere**: no
  aarch64 branch protection, no explicit Windows flags, two subprojects built
  without the hardening flags, and no sanitizer, fuzzing or static-analysis
  job in CI.

### Top recommendations, ranked by value over effort

| # | Change | Why | Effort |
|---|--------|-----|--------|
| 1 | Refuse renderer-requested navigation to `about:history` and other privileged pages in the shell (S2) | A compromised renderer is handed the full history | small |
| 2 | macOS: stop renderers writing the config directory and reading the state directory; never start remote control from a config file change the shell did not write (S3) | Renderer escape to full control of every tab on macOS and Windows | small |
| 3 | Drop or restrict the cross-process popup `call` relay (S1) | Script execution in another tab's origin from a compromised popup | small–medium |
| 4 | Pass the hardening flags to the `wuffs` and `plmpeg` subprojects (B2) | `pl_mpeg` parses hostile bytes with no stack protector or FORTIFY in CI, AppImage, macOS and Windows builds | small |
| 5 | Add `-ftrivial-auto-var-init=zero`, `-fwrapv`, `-mbranch-protection=standard`, explicit Windows ASLR/NX flags (B1) | Cheap, measurable mitigation classes | small |
| 6 | Replace the renderer process when the top-level site changes (S6) | A renderer compromised on site A keeps handling site B | medium |
| 7 | Narrow the Landlock read set: `$XDG_RUNTIME_DIR`, `/proc`, `/dev/shm` (S4) | Credentials and other applications' data are readable today | medium |
| 8 | Add CodeQL and an ASan+UBSan CI job (B3) | No automated memory-safety signal exists for a 285k-line C tree | medium |
| 9 | Parse each IPC message once with `rjson.c` instead of the flat key scanner (D5) | Exact semantics on data from the sandboxed renderer; removes a C and a Rust implementation | medium |
| 10 | Delete the dead exports and inert config keys; make 111 functions `static` (section 3) | ~225 lines, and the compiler then guards them | small |
| 11 | One `pattern`-attribute matcher (D6) | `:invalid`, form submission and `validity` can disagree today | small |
| 12 | Use the in-tree lexbor encoding module instead of `g_convert` (M2) | WHATWG-exact decoding with no new dependency | medium |

### Method and coverage

Two parallel audits plus spot checks against the code. Findings carry
`path:line` for lines that were opened.

| Area | Coverage |
|------|----------|
| Symbol table of 11,836 functions, 1,903 header prototypes, write-only fields, clone windows | automated over all first-party files |
| `clang -fsyntax-only -Wall -Wextra -Wunused -Wunused-macros` | 100 of 106 `src/*.c` |
| `clang --analyze` (`core`, `unix`, `security`, four alpha bounds checkers) | 39 of 59 files attempted; 255 warnings, 227 noise, the other 28 read and all false positives |
| `src/ipc_http.c`, `src/mp4.c`, `src/image_ico.c`, seccomp and Landlock sections of `src/security.c` | read in full |
| `src/rproc_http.c`, `src/gtk/procwindow.c`, `src/gtk/procview.c`, `src/gtk/remotectl.c`, `src/gtk/webdriver.c`, `src/audio/main.c`, `src/woff2.c`, `src/cookies.c`, `src/csp.c`, `src/webcrypto.c`, `src/config.c` | read in substantial part |
| `src/js.c` | **about 1%** (popup channel, `JS_ReadObject` call sites). The largest unreviewed risk |
| `src/layout.c`, `src/paint.c`, `src/css.c`, `src/dom.c` (node lifetime), `src/webgpu.c`, `src/wasm.c`, `src/video.c`, `src/cache.c`, `src/image_webp/avif/jxl.c`, CORS / mixed-content / iframe-sandbox logic | **not security-reviewed** |
| Forked trees (`lexbor`, both QuickJS, `web-text-lib`, `wamr`) | update log only |

Nothing was built: the container lacks GTK 4, libcurl and eleven other
required packages. Section 5 therefore describes the flags the build files
request, not flags observed in a binary. "Lines removable" figures are
estimates from function spans.

## 2. Security findings

"Compromised renderer" means code execution inside one
`nordstjernen-renderer`.

| # | Finding | Attacker | Severity |
|---|---------|----------|----------|
| S1 | Shell takes a tab's origin from the renderer | compromised renderer | high |
| S2 | Renderer can request `about:history` and receives it | compromised renderer | medium |
| S3 | macOS/Windows: renderer can rewrite the config and read the remote-control token | compromised renderer | high on those platforms |
| S4 | Landlock read set wider than documented | compromised renderer | medium |
| S5 | Loopback reachable; `--webdriver` unauthenticated | compromised renderer | medium with `--webdriver` |
| S6 | Renderer process survives cross-site navigation | compromised renderer | medium |
| S7 | Cross-process payloads go through `JS_ReadObject` | compromised renderer | low–medium, unverified |
| S8 | seccomp allow-list broader than needed | compromised renderer | low |
| S9 | Clipboard writes and downloads on the renderer's word | web page / renderer | low |
| S10 | TLS-override retry checks HSTS for the wrong host | network, opt-in setting | low, unverified |
| S11 | Private-mode fallback directory in `/tmp` | local user | low |
| S12 | Signed-overflow UB in MP4 timestamps | malicious file | low |
| S13 | Windows mitigation status reported without checking results | — | info |
| S14 | Unreachable http(s) fetch path in the audio helper | — | info |

### S1. The shell's idea of a tab's origin is whatever the renderer says

`v->current_url` is set from the renderer's `/open` reply
(`src/gtk/procview.c:1291`, `:3804`) and updated from every tick
(`procview.c:2923-2937`). Redirects happen inside the renderer, so the shell
has no independent knowledge. Decisions built on that value:

- **Opener scripting.** `pw_popup_call_allowed`
  (`src/gtk/procwindow.c:2302-2319`) relays a `call` op when the popup's
  reported URL matches the opener's origin. `ns_popup_deliver_call`
  (`src/js.c:28374-28432`) then walks a key path from the global object and
  calls the function it finds with the sender's arguments. `eval` is a
  function on the global. Result: a compromised popup runs script in the
  opener's origin, in another process.
- **`postMessage` origin** (`procwindow.c:2291-2299`), **stored permission
  grants** (`procview.c:3570-3587`), **download and popup permissions**
  (`procwindow.c:2005-2008`, `:2392-2395`) and the **WebAuthn origin check**
  (`src/gtk/ns_webauthn.c:289-292`) are forgeable the same way.
- **URL bar** shows the same value (`procview.c:3824`).

Fix: the complete answer is the network broker already planned in
`docs/tab-isolation.md`. Until then, drop the `call` relay or limit it to
tab pairs whose URLs the shell supplied and that have not navigated; replace
the renderer when the reported site differs from the requested one; refuse
WebAuthn and permission prompts when the two differ.

### S2. A renderer can pull the browsing history

`do_load_full` attaches `ns_history_html_page()` to any load of
`about:history` (`procview.c:3084-3085`). A renderer-supplied `X-Nav` reaches
`do_load` unfiltered (`procview.c:3790-3793`, `:3840-3843`), and the load goes
to the same process. The rule that web content may not navigate to privileged
pages (`src/libnordstjernen.c:964-977`) is enforced inside the renderer.

Fix: in the shell, refuse renderer-initiated navigation to privileged
`about:` pages, `view-source:`, and `file:` from a non-`file:` page. Attach
the history HTML only to loads the user started.

### S3. macOS and Windows: config rewrite and remote-control token

The Seatbelt profile is `(allow default)` with `(deny file-write*)` and
exceptions that include the whole config directory for every process
(`src/security.c:1306-1338`); reads are unrestricted. Windows has no
filesystem confinement. A compromised renderer can:

- set `remote_control = true` in `nordstjernen.conf`; the shell's file
  monitor starts the server with no prompt (`src/gtk/remotectl.c:1012-1028`),
  and the renderer reads the port and token and drives every tab;
- set proxies, `tls_allow_insecure_override`, `safe_browsing` and `doh_url`,
  bypassing the confirmation dialog that guards only the IPC path;
- edit the permission store (`src/gtk/permissions.c:81`) and session store.

`SECURITY.md` says the token is "stored in the state directory, which
renderers cannot read". That holds on Linux only.

Fix: on macOS deny reads of the state directory and drop the config
directory from the renderer's write list, keeping `cookies/` as Landlock
does. On every platform, ignore `remote_control` turning on through a file
change the shell did not write.

### S4. The Landlock read set is wider than documented

Read access covers all of `$XDG_RUNTIME_DIR` (`src/security.c:653-654`),
`/proc`, `/sys`, `/etc` and `/dev/shm` (`:598-604`). On a typical desktop the
runtime directory holds gvfs mounts, the document portal,
`containers/auth.json` and, on some distributions, Kerberos caches. Sockets
there are not connectable because seccomp refuses `socket(AF_UNIX)`.

Fix: grant only the runtime-directory entries a renderer needs (none are
evident), narrow `/proc` to `/proc/self` plus the global files glibc and Mesa
read, and drop `/dev/shm` unless a driver needs it. Check that printing still
works afterwards (`src/rproc_http.c:2134-2136` writes there).

### S5. Loopback and WebDriver

seccomp allows `AF_INET`/`AF_INET6` sockets and `connect`
(`src/security.c:1125-1137`); Private Network Access is enforced inside the
renderer. `--webdriver` has no authentication and its Host/Origin checks
(`src/gtk/webdriver.c:429-459`) only stop web pages. `--remote-control` is
token-protected with a constant-time compare and is fine on Linux.

Fix: give WebDriver the same token scheme.

### S6. One renderer per tab, not per site

`worker_main` reuses the process for every load unless the capture flag
changes (`procview.c:1249-1262`). All renderers also share one cookie
database, HTTP cache and localStorage tree, so a compromise reads every
site's `HttpOnly` cookies and can poison cached scripts across restarts.

Fix: replace the renderer when the top-level site changes; the retire/carry
code for the capture case already exists (`procview.c:1218-1231`). Storage
isolation needs the broker.

### S7–S14

- **S7.** `ns_popup_deserialize` (`src/js.c:27740-27753`) feeds bytes from
  another renderer to `JS_ReadObject`. Bytecode and SharedArrayBuffer are off
  and size is capped; no specific bug was found. Carry structured-clone data
  in JSON plus a typed-array side channel, or fuzz the reader out of tree.
- **S8.** Default action is `EPERM` and the dangerous calls are absent, which
  is sound. Worth trimming: `ioctl` is allowed for every request except two
  (`src/security.c:1174-1193`) and is the largest kernel surface with GPU
  devices open; `prctl`, `pidfd_open`, `memfd_secret`, `mlockall`,
  `sched_set*`, `bind`, `symlink`, `link` are not needed by a renderer.
- **S9.** The clipboard is set to whatever a frame returns in `X-Clipboard`
  (`procview.c:3949-3952`) with no shell-side gesture check.
- **S10.** With `tls_allow_insecure_override` on, the retry checks HSTS for
  `req->url`'s host, not the redirect target that failed
  (`src/net.c:6682-6707`). Use `CURLINFO_EFFECTIVE_URL` and do not follow
  redirects on the unverified retry.
- **S11.** If `g_dir_make_tmp` fails, the private profile falls back to
  `/tmp/nordstjernen-private-<g_random_int()>` (`src/net.c:487-494`), which
  succeeds when another user already made the directory. Fail instead.
- **S12.** `*dts += dur` on `int64_t` taken from `tfdt` (`src/mp4.c:466-473`)
  and an unbounded double-to-`gint64` conversion (`src/video_mp4.c:137`).
  Neither indexes memory.
- **S13.** `ns_status.win32_mitigations` is set before
  `SetProcessMitigationPolicy` is called and results are ignored
  (`src/security.c:1408-1450`). The Windows renderer also inherits the full
  environment (`src/rproc_http.c:444-446`) where the POSIX path filters it.
- **S14.** The shell forwards only `file://` URLs under the audio cache, but
  the helper still carries a libcurl http(s) path
  (`src/audio/main.c:1346-1393`). Deleting it lets the helper drop its
  network syscalls.

### Checked and found sound

IPC transport (a `socketpair`, `close_range`, environment allow-list); the
sealed-`memfd` framebuffer and its geometry checks; IPC framing limits; the
config-over-IPC allow-list and confirmation dialog; download staging
(`O_NOFOLLOW`, inode re-check, name sanitising); the print hand-off; the
in-memory bytecode cache (nothing on disk to poison); cookie prefix,
`Secure`, `SameSite` and public-suffix rules; TLS verification and the
`https`-only redirect rule; extension path checks; token randomness
(`getrandom`/`BCryptGenRandom`/`RAND_bytes`); signal handlers. No `sprintf`,
`strcat`, `gets`, `alloca`, VLA, `system` or `popen` in first-party code;
the nine `strcpy` calls copy literals or length-checked strings.

## 3. Unused code

Negative results, each from a tool: 0 unused static functions, 0 prototypes
without a definition, 0 headers never included, 0 `.c` files outside the
build, 0 `#ifdef NS_*` branches that cannot be enabled, 0 unread meson
options.

| # | Location | What | Proposed change |
|---|----------|------|-----------------|
| U1 | 111 functions: `src/js_canvas.c` 39, `src/css.c` 22, `src/js_canvas_api.c` 7, `src/js.c` 7, others 36 | Non-static, referenced only in their own file | Make `static`, delete ~125 prototype lines; `-Wunused-function` then guards them |
| U2 | `src/cookies.c:1316` `ns_cookie_store_site_json` | No caller | Delete (27 lines) |
| U3 | `src/net.c:2641-2671` `ns_net_proxy_override`, `ns_net_http_proxy`, `ns_net_https_proxy`, `ns_net_no_proxy`, `ns_net_ca_bundle_path` | No caller | Delete (23 lines) |
| U4 | `src/net.c:8889` `ns_net_fetch_blocking` | No caller | Delete |
| U5 | `src/js_cage.c:47-62` three cage getters; `src/js.c:74748` `ns_js_storage_partition`; `src/rproc_inproc.c:237` `ns_rproc_single_process_enabled` | No caller | Delete, unless the cage getters are wanted for the heap-hardening benchmarks |
| U6 | `src/config.h:44-80` | Seven keys parsed, saved and dumped but read nowhere: `ai_model_mirror`, `reduced_motion`, `camera_enabled`, `microphone_enabled`, `window_width_px`, `window_height_px`, `layout_viewport_px` | Remove or wire up. `camera_enabled = false` and `microphone_enabled = false` look like switches and do nothing |
| U7 | `src/debuglog.h:23`, `src/gtk/procwindow.c:4097`, `src/gtk/remotectl.c:70`, `src/js.c:9790`, `src/libnordstjernen.c:73` | Struct fields written, never read | Delete |
| U8 | `src/libnordstjernen.h:82` `ns_browser_set_viewport_width` | Public API with no caller, binding or documentation | Drop or document |
| U9 | `src/libnordstjernen.h:104,110,241` | Exported, called only inside `libnordstjernen.c`, undocumented | Document or make `static` |
| U10 | `src/about_style.h:108` `NS_ABOUT_ICON_ERROR` | Macro never expanded | Delete |
| U11 | `data/packaging/nordstjernen.spec` | Third RPM spec, at 1.0.29, referenced by nothing | Delete |
| U12 | `src/ipc_http_rs/lib.rs` (678 lines), `-Dipc_rust` | Second implementation of `src/ipc_http.c`; built by no CI job, so it can drift unnoticed | Add one CI job or remove |
| U13 | `scripts/build-logo-gif.sh`, `scripts/gen-android-icon.py`, `scripts/gen-windows-icon.py` | Referenced nowhere | Document or delete |

Stale documentation:

- `CLAUDE.md`, `AGENTS.md` and `docs/Windows.md` name `ns_html_decode_body`
  and say there is no BOM, HTTP-charset or meta-charset sniffing. The
  function is `ns_html_decode_body_full` (`src/html.c:733`) and it does all
  three before falling back to uchardet. The code is right.
- `docs/Benchmarking.md:431` cites `ns_anim_observe`;
  `docs/HTML-compatibility.md:315` cites `NS_RPROC_MSG_HOVER`. Neither
  exists.
- `nordstjernen.spec:54` recommends `mpv`, which nothing invokes.
- `meson.build:102` prints "LTO + stripped + hardened"; `b_lto` is never set.
- `AGENTS.md` and `CLAUDE.md` are two hand-maintained copies with 287
  differing lines.

## 4. Duplicate code

| # | Cluster | Where | Size | Proposed single home |
|---|---------|-------|------|----------------------|
| D1 | CSS values scanned on raw strings instead of through the tokenizer | `src/css.c` has 9 quote-aware scanners (e.g. `:2173`, `:20592`, `:22825`) and 23 paren-depth scanners (e.g. `:3391`, `:11253`, `:38261`); `src/css_color.c:621` has a private tokenizer. `src/css_syntax.c` has 12 callers | ~1,200 lines | One scan helper (skip string / comment / balanced block). The depth-only scanners mishandle `)` and `,` inside quoted strings, so this is also a correctness item |
| D2 | Five `calc()` evaluators | `src/css.c:3418-3900`, `src/css_prop_syntax.c:113-893`, `src/css_color.c:962-1046`, `src/css_media.c:362-451`, `src/css_shape.c:152-235` | ~800 lines outside `css.c` | One evaluator over the component tree with a caller-supplied identifier environment |
| D3 | Four SVG path parsers, three arc converters | `src/svg.c:914`, `src/js_canvas.c:3098`, `src/js.c:42309`, `src/css_shape.c:1594`; arcs at `svg.c:850`, `js_canvas.c:3053`, `js.c:42269` | 531 lines, ~330 removable | One parser in `svg.c` emitting a command array |
| D4 | JSON string escaping, six times | `src/cookies.c:1213`, `src/js.c:27664`, `src/net.c:5778` and `:5987`, `src/ipc_http.c:492`, `src/gtk/rjson.c` | ~85 removable | `ns_json_append_string` in `src/json_string.c` |
| D5 | JSON reading, four ways | `src/gtk/rjson.c` (real parser, shell only); `json_find_value` in `src/ipc_http.c:546`, used ~195 times through `json_get_*` including by the shell on renderer replies; `src/html_lexbor.c:308`; the Rust twin | — | Move `rjson.c` to `src/` and parse each IPC message once. `json_find_value` matches a key at any nesting depth and rescans the body per lookup |
| D6 | `pattern` attribute matching, three behaviours | `src/forms.c:223` (PCRE, length-capped), `src/css.c:25758` (PCRE, uncapped, runs during selector matching), `src/js.c` near `:43000` (JS RegExp first) | ~45 lines | One function using the engine's RegExp, as the specification requires |
| D7 | URL splitting outside `ns_url_*` | `src/ext.c:95` (hand-rolled; its host keeps userinfo and port, so match patterns compare against `user@host:port`); `src/csp.c:266-300`; GUri in `cookies.c:684`, `history.c:252`, `net.c:3961`, `netutil.c:202` | ~120 lines | Engine files use `ns_url_parts_new`. Fix `ext.c:95` first |
| D8 | Eight CSS number serialisers | `css_anchor.c:603`, `css_color.c:1131/1163/1182`, `css_filter.c:207`, `css_shape.c:268`, `css_prop_syntax.c:896`, `css.c:28823` | ~135 lines | `ns_css_append_number` |
| D9 | Typed array to byte span | `webgl.c:683`, `js.c:12528`, `webaudio.c:60`, `js_canvas_api.c:1479`, `webgpu.c:2032`, plus 20 open-coded sites | ~250 lines | One `ns_js_buffer_view` in `js_internal.h`. The `off + len > total` check is security-relevant and is re-derived at each site |
| D10 | "Get string property" helper | `js.c:1095`, `js.c:12615`, `webgpu.c:2577`, `ext.c:520`, `webaudio.c:49` | ~70 lines | Export `ns_js_get_string_prop` |
| D11 | ASCII whitespace predicates | 13 small functions and 44 open-coded tests in 16 files | ~100 lines | Two inline predicates (HTML and CSS whitespace). `g_ascii_isspace` is not a drop-in: it accepts `\v` |
| D12 | Inline FNV-1a | 17 sites, e.g. `css.c:2607`, `dom.c:959`, `engine.c:901`, `layout.c:7075` | ~90 lines | `ns_fnv1a32/64` in one header |
| D13 | Box blur, three times | `js_canvas.c:1156`, `paint.c:6772`, `paint.c:4541` | ~45 removable | Canvas shadow blur calls the `paint.c` blur |
| D14 | Same-named statics in two files | `overflow_kw_clips` (`layout.c:9021`, `paint.c:277`), `keyword_value` (`css.c:12960`, `layout.c:20279`), `parse_angle_deg` (`css.c:7966`, `css_shape.c:879`), `ns_fetch_omits_credentials` (`js.c:9990`, `net.c:6426`) and eleven more | ~200 lines | Move each to the header its two users already share |
| D15 | PCM append in the OS decoders | `media_os_android.c:312`, `media_os_gst.c:440`, `media_os_mf.c:850` | ~60 removable | One helper in `media_os.c` |
| D16 | SQLite prepare/bind/step boilerplate | 39 `sqlite3_prepare_v2` sites; exact clones at `idb.c:490` and `:1056` | ~150 lines | `ns_db_prepare`, `ns_db_column_dup` in `src/db.c` |

## 5. Build hardening as configured

| Control | State |
|---------|-------|
| `_FORTIFY_SOURCE` | 3, falling back to 2 (`meson.build:42-45`) |
| Stack protector, stack-clash, `-fcf-protection=full` | on where supported (`meson.build:48-50`) |
| PIE, RELRO + BIND_NOW, noexecstack, separate-code | on for ELF |
| `-mbranch-protection` (aarch64 PAC/BTI) | **not set** |
| `-ftrivial-auto-var-init=zero` | **not set** |
| `-fwrapv`, `-fno-strict-aliasing`, `-fno-delete-null-pointer-checks` | **not set** |
| LTO | **not enabled**, despite the build message |
| Windows ASLR / high-entropy VA / NX / CFG | not requested; relies on MinGW defaults. No Authenticode step |
| macOS | hardened runtime only with a Developer ID. `allow-jit`, `allow-unsigned-executable-memory` and `disable-library-validation` are granted to the shell, renderer and audio helper alike (`packaging/macos/entitlements.plist`) |
| Subprojects `wuffs`, `plmpeg` | **built without the hardening flags** unless the packager's `CFLAGS` supply them (Debian and RPM do; CI artifacts, AppImage, macOS and Windows do not) |
| Sanitizers, fuzzing, static analysis in CI | **none** |
| Dependency scanning | Dependabot for GitHub Actions only |
| Supply chain | Actions pinned by SHA, wraps and downloads checksummed. Releases carry `SHA256SUMS` but no signature, SBOM or provenance attestation |
| Vendored engines | QuickJS (both), lexbor and Wuffs updated within two weeks. `pl_mpeg`, `minimp3` and WAMR have no pinned version or update log |

Proposed changes:

- **B1.** Add `-ftrivial-auto-var-init=zero`, `-fwrapv`,
  `-fno-delete-null-pointer-checks`, `-fstrict-flex-arrays=3`,
  `-mbranch-protection=standard` on aarch64 and
  `-Wl,--dynamicbase,--nxcompat,--high-entropy-va` on Windows. Add
  `-Wimplicit-fallthrough` and `-Walloca`.
- **B2.** Give the two subprojects the hardening flags, or move them under
  `src/` like the other vendored code.
- **B3.** Enable CodeQL for C. Add an ASan+UBSan job that runs the headless
  binary over `data/render-tests/`; a crash-only run is not a test suite.
- **B4.** Fix or remove the LTO claim. If LTO is turned on, Clang's
  `-fsanitize=cfi` becomes available for the renderer.
- **B5.** Give the macOS renderer its own entitlements file and drop
  `disable-library-validation` unless a concrete dylib needs it.
- **B6.** Sign `SHA256SUMS` and attach a build-provenance attestation.
- **B7.** Record a pinned upstream commit for `pl_mpeg`, `minimp3` and WAMR
  in `docs/vendored-engine-updates.md`.

## 6. Code a library could replace

No hand-rolled SHA, MD5, CRC, IDNA, Unicode normalisation, public-suffix or
regex code exists in first-party files; digests go through OpenSSL or
`GChecksum`.

### Worth changing

| # | Code | Replacement | Verdict |
|---|------|-------------|---------|
| L1 | `src/xml.c` (612 lines): XML parser for XHTML, `DOMParser` and XHR | tighten in-tree; **expat** only if exact conformance becomes a goal | Researched in section 10. The in-tree parser accepts 19 of 22 malformed test documents and mishandles 3 of 7 well-formed ones; expat gets all 29 right. But expat is not a hardening step: it had eight security releases in 2026, including out-of-bounds writes and a use-after-free, and Firefox runs it inside a WebAssembly sandbox |
| L2 | `json_find_value`, `src/ipc_http.c:546-630`, and its Rust twin | in-tree `src/gtk/rjson.c` | Win, no new dependency (D5) |
| L3 | Budgeted base64 for `data:` URLs, `src/net.c:4353-4431` | GLib `g_base64_decode_step` in chunks | ~55 lines; confirm the forgiving-base64 rules are kept |
| L4 | UTF-8 encoding of a code point, `src/js.c:24206-24228` | `g_unichar_to_utf8` | 23 lines |
| L5 | `pattern` matching through PCRE | the JS engine's RegExp | D6 |
| L6 | Legacy charset decoding through `g_convert` (8 call sites) | lexbor's encoding module, already in `src/lexbor/source/lexbor/encoding` and unused | See M2 |

### Assessed and better left in-tree

`src/woff2.c` (built only when FreeType lacks Brotli; the alternative is a
packaging change, not a library), `src/mp4.c` and `src/video_mp4.c` (built
only without libav), `src/content_decode.c` (libcurl has no decompressed-size
cap), `src/datetime.c`, `src/css_color.c` (lcms2 would replace ~150 of 2,314
lines), `src/image_ico.c`, `src/json_string.c`, `src/gtk/rjson.c`,
`src/csp.c`, `src/cookies.c`, `src/cache.c`, `src/idb.c`, `src/font.c`,
`src/i18n.c`, `src/mat4.h`, `src/mathml.c`, `src/svg.c`,
`src/safebrowsing_google.c`. `src/pdf.c`, `src/ws.c`, `src/spellcheck.c` and
`src/webcrypto.c` are already thin wrappers over a linked library.

## 7. Libraries and mechanisms to harden the browser

Already in place: libseccomp, Landlock, `PR_SET_NO_NEW_PRIVS`, the QuickJS
heap cage, Wuffs for image decoding, glslang shader validation, SQLite in
defensive mode with `trusted_schema` off, curl HSTS, ECH and DoH, and
`mallopt(M_PERTURB)` on glibc.

| # | Candidate | What it adds | Verdict |
|---|-----------|--------------|---------|
| H1 | **A hardened allocator for the renderer**: GrapheneOS hardened_malloc (Linux 64-bit), LLVM Scudo, or mimalloc in secure mode | Guard pages, quarantine, randomised slots, out-of-line metadata. Today the only allocator hardening is freed-memory poisoning (`src/security.c:1516`), and only on glibc. The JS heap is caged, but the DOM, layout and CSS live in the system heap | **Recommended.** Highest-value library change. Measure with `docs/Benchmarking.md` first; ship as a build option for the renderer only |
| H2 | **OpenType Sanitizer (OTS)** | Validates and rewrites web fonts before FreeType and HarfBuzz see them. Fonts are attacker bytes handed to two large C libraries (`src/font.c:542`) | **Recommended to evaluate.** C++ and not widely packaged, so it would be a wrap. Chrome and Firefox both use it for this reason |
| H3 | **expat** | Exact XML well-formedness (L1) | **Not a hardening measure**; see section 10 |
| H4 | **libfido2** (Yubico, C) | A maintained CTAP implementation for roaming authenticators, if WebAuthn on Linux is to go beyond what exists | Evaluate when Linux WebAuthn is next touched |
| H5 | **More Wuffs** | The vendored release also carries memory-safe deflate, gzip, zlib, LZW and JSON decoders | Low priority: zlib is not the weak point |
| H6 | **Certificate revocation** | No `CURLOPT_SSL_VERIFYSTATUS` or CRL use was found, so a revoked certificate is accepted | Open design question; see M4 |
| H7 | **C23 `<stdckdint.h>`** (`ckd_mul`, `ckd_add`) | One spelling for overflow-checked size arithmetic. `c_std` already prefers `gnu2x` | Adopt for new code |
| H8 | **Platform sandbox APIs**, not libraries | Windows: restricted token, Job Object, AppContainer, win32k lockdown. macOS: a deny-default Seatbelt profile for the renderer. FreeBSD: Capsicum. OpenBSD: `pledge`/`unveil` | Windows is the largest gap in the tree: the renderer has no filesystem confinement there |
| H9 | **Fil-C** | A memory-safe C compiler. A Fil-C build of the renderer would be a useful experiment | Experiment only; not a shipping configuration |

Not recommended: libsodium (OpenSSL already provides `CRYPTO_memcmp` and
`OPENSSL_cleanse`, both in use), safeclib / Annex K, a second TLS stack,
bubblewrap or minijail (the in-process Landlock and seccomp setup is tighter
and has no setuid helper).

## 8. Dependencies from the Mozilla project

`CLAUDE.md` forbids Mozilla/Gecko engine code. That rules out SpiderMonkey,
Stylo, WebRender and the Servo crates (`html5ever`, `cssparser`, `selectors`,
`rust-url`). It does not obviously rule out standalone libraries and data
that Mozilla publishes for others to use. **Whether those count is the
owner's decision**; the table assumes they are allowed and judges each on
merit. Licence terms and packaging state below are from memory and should be
confirmed before adopting anything.

Already used, of Mozilla origin: **uchardet** (derived from Mozilla's
universal charset detector), the **Public Suffix List** through libpsl, and
Mozilla's **CA root programme** through the system trust store.

| # | Candidate | Language, licence | What it would replace or add | Verdict |
|---|-----------|-------------------|------------------------------|---------|
| M1 | **cubeb** | C/C++, ISC | Audio output and capture, replacing SDL2 in `src/audio/main.c` and `src/mic.c`. SDL2 is a full game library used here for one audio device | Plausible. Smaller and audio-only, with the same backend coverage. Cost: CMake build and uneven distribution packaging, so it would be a wrap. Worth doing only if SDL2 becomes a problem |
| M2 | **encoding_rs** through `encoding_c` | Rust, MIT/Apache-2.0 | WHATWG-exact legacy decoding in place of `g_convert` | **Not recommended**: lexbor's encoding module is already in the tree and does the same job in C (L6). Use that |
| M3 | **PDF.js** | JavaScript, Apache-2.0 | Replaces poppler (`src/pdf.c`). PDF parsing would move from a large C++ library to JavaScript inside the caged engine and the sandbox | Researched in section 11: unmodified PDF.js 6.5.438 opens and renders correctly in this engine once one engine bug is fixed. Worth a prototype behind a build option |
| M4 | **CRLite** (`rust-cascade`) and Mozilla's revocation data | Rust, MPL-2.0 | Offline certificate revocation checking (H6) | Technically the best design for revocation. But the filters are fetched from Mozilla's servers on a schedule, which sits badly with "does not phone home", and it needs Rust with crates. Decide the revocation question first |
| M5 | **mp4parse-rust** (`mp4parse_capi`) | Rust, MPL-2.0 | A memory-safe MP4 parser in place of `src/mp4.c` | Not recommended. `mp4.c` is 583 bounds-checked lines built only without libav, and this needs cargo |
| M6 | **chardetng** | Rust, MIT/Apache-2.0 | Firefox's current charset detector, in place of uchardet | Not recommended. uchardet is packaged everywhere and runs only as the last fallback |
| M7 | **NSS / NSPR** | C, MPL-2.0 | TLS and crypto | No. OpenSSL is already linked through libcurl and WebCrypto is written against it |
| M8 | **RLBox** | C++17, MIT | Firefox's framework for sandboxing third-party libraries by compiling them to WebAssembly | The framework does not fit a C tree. The idea does: `pl_mpeg` and `minimp3` could run under the in-tree WAMR. Low priority while the renderer sandbox contains them |
| M9 | **Readability.js** | JavaScript, Apache-2.0 | A reader mode | A feature, not hardening. Small and self-contained if a reader mode is ever wanted |
| M10 | **authenticator-rs** | Rust, MPL-2.0 | CTAP for WebAuthn | Prefer libfido2 (H4), which is C |
| M11 | **Tracking-protection lists** (`shavar-prod-lists`) | data | Content blocking | The lists originate from Disconnect and carry their own licence. Check it against this project's dual licence before any use |
| M12 | **Tooling**: `rr`, `sccache`, the Grizzly fuzzing harness, Lithium test-case reducer, MDN browser-compat-data | — | Debugging, CI caching, out-of-tree fuzzing, compatibility reference | Useful and free of the engine-code question, since none of it ships. Grizzly plus Lithium is the practical way to fuzz the renderer without adding tests to the tree |

Summary: no Mozilla library is a clear win as a new linked dependency today.
cubeb is the only C one that fits, and it solves no present problem. PDF.js
is the exception: it is not linked at all, it runs as script, and the trial
in section 11 shows it working. Most of the rest are Rust crates that need
cargo, which the tree deliberately avoids (`-Dipc_rust` uses no crates).

## 9. Suggested order of work

1. **Shell-side trust fixes**: S2, S3, S1, then S5 and S9. Small diffs in
   `src/gtk/` and `src/security.c`, each a `Security-Fix:` commit that
   Northstar and Southstar should check too. Update `SECURITY.md` where it
   overstates the macOS and Windows position.
2. **Build flags**: B1, B2, B4, B7.
3. **Dead code**: U2–U7, U10, U11, the stale documentation, then U1.
4. **CI signal**: B3, and a decision on U12.
5. **Sandbox narrowing**: S4, S8, S6.
6. **De-duplication with behavioural effect**: D5, D6, D7, D9.
7. **De-duplication for size**: D3, D4, D8, D10–D16; D1 and D2 when next
   touched.
8. **Library decisions for the owner**: H1 (hardened allocator), H2 (OTS),
   H6/M4 (revocation), L1 (tighten `src/xml.c`, section 10), M3 (PDF.js
   prototype, section 11).
9. **Further review**: `src/js.c` typed-array and structured-clone paths and
   DOM node lifetime were not covered and are where a memory-safety bug is
   most likely to be.

## 10. Follow-up: `src/xml.c` and expat

The first version of this report called expat "hardened XML parsing". That
was wrong, and this section replaces it. expat is the more correct parser by
a wide margin, but it is not the safer one.

### What was measured

The browser was built at this commit and 29 small documents were run through
`DOMParser.parseFromString(…, "application/xml")` in headless mode, then
through expat 2.9.0 (Python's `pyexpat`).

| Group | In-tree `src/xml.c` | expat |
|-------|---------------------|-------|
| 22 documents that are not well-formed | **19 accepted**, 3 rejected | 22 rejected |
| 7 well-formed documents | 4 right, **3 wrong** | 7 right |

Malformed input the in-tree parser accepts: text or a second element after
the root (`<a/>junk`, `<a/><b/>`, silently dropped), duplicate attributes,
`<` in an attribute value, undefined entities, a bare `&`, `&#xZZ;`,
references to U+0000 and U+0001, raw control characters, `]]>` in text, `--`
in a comment, unbound element and attribute prefixes, attributes with no
space between them, an XML declaration inside the document, and a name
containing U+00D7. It rejects only a mismatched end tag, an unquoted
attribute and a name starting with a digit.

Well-formed input it gets wrong:

- an internal entity (`<!ENTITY e "hello">` … `&e;`) is left as the literal
  text `&e;`, because the DOCTYPE internal subset is skipped
  (`src/xml.c:285-289`). Illustrator-exported SVG uses such entities for its
  namespace URIs;
- an attribute default from `<!ATTLIST>` is not applied;
- 300 nested elements are rejected (`XML_MAX_DEPTH` is 256, `src/xml.c:192`),
  because the parser recurses per element.

Two further points from reading the code:

- **There are two XML scanners.** `ns_xml_well_formed`
  (`src/html_lexbor.c:518-605`) is a separate tag-balance checker run before
  `ns_xml_parse` for XHTML documents and frames. `DOMParser` and XHR call
  only the parser, so the same bytes are judged differently by path. Its
  root-namespace lookup (`html_lexbor.c:497-515`) scans the start tag for
  `xmlns` after whitespace without regard to quotes, so the text
  ` xmlns='…'` inside another attribute's value is taken as the namespace.
- **Entity scanning is quadratic.** Each `&` without a `;` makes
  `xml_decode_text` search to the end of the text run
  (`src/xml.c:129`). Measured: 100,000 ampersands take 75 ms, 200,000 take
  284 ms, 400,000 take 966 ms. expat rejects the same input in under 1 ms.

No memory-safety problem was found in `src/xml.c`; it was read in full.

### What expat would cost

expat's 2026 record, from its release announcements:

| Release | Fixes |
|---------|-------|
| 2.7.4 (January) | NULL dereference, integer overflow |
| 2.7.5, 2.8.0, 2.8.1 | security fixes, including insufficient hash-salt entropy and a quadratic attribute check |
| 2.8.2 (June) | 13 CVEs: nine integer overflows, one out-of-bounds write, three missing integrity checks |
| 2.8.4 | out-of-bounds read, use-after-free with custom encoding handlers, quadratic `storeAtts()` |
| 2.9.0 (October) | two CVEs, one a missing length check in `XML_ParseBuffer` |
| 2.10.0 (2 November) | four CVEs |

The maintainer closed the project to new vulnerability reports for part of
the summer. Firefox, the largest browser user of expat, runs it inside an
RLBox WebAssembly sandbox for this reason. Chrome is replacing its XML parser
(libxml2) with a memory-safe one written in Rust. Neither major engine now
runs a C XML parser unsandboxed on web content.

Linking expat would therefore add a memory-unsafe parser with a heavy
advisory stream to the renderer, and make the browser's exposure depend on
how quickly each distribution ships expat updates. The sandbox contains it,
but section 2 shows what a compromised renderer can still reach.

What expat would give: all 29 cases right, internal entities with built-in
amplification limits, exact error line and column for `parsererror`, an
iterative element loop with no depth limit, and namespace triplets
(`XML_SetReturnNSTriplet`) that map directly onto `xml_apply_namespace`.
About 350 lines would go, counting `ns_xml_well_formed`. Packaging is not an
obstacle: fontconfig already requires it on every desktop platform
(`libfontconfig-dev` depends on `libexpat1-dev`), and the Android sysroot
already builds it (`android/scripts/build-deps.sh:21`). XHTML named entities
would need a small in-memory DTD generated from lexbor's table, served
through expat's external-entity handler.

### Recommendation

1. **Tighten the in-tree parser.** Make the parser the single authority and
   delete `ns_xml_well_formed`. Add the missing well-formedness checks
   (content after the root, duplicate attributes, `<` in attribute values,
   undefined entities and bare `&` as errors, character-reference syntax and
   range, legal characters, `]]>` in text, unbound prefixes, whitespace
   between attributes). Fix the quadratic scan by bounding the search for
   `;`. Replace the recursion with an explicit stack. Roughly 200 lines
   added and 110 removed, no new dependency, no new attack surface.
2. **Internal entities** are the one part that is risky to write by hand,
   because expansion needs amplification limits. Either support only
   non-nested internal entities with a hard size cap, or report documents
   that declare them as unsupported. Decide when item 1 is done.
3. **Revisit expat only if exact XML conformance becomes a goal**, and then
   treat it as a correctness dependency that needs the sandbox, not as
   hardening.

Sources: [Expat 2.8.2 released](https://blog.hartwork.org/posts/expat-2-8-2-released/),
[Expat 2.7.4 released](https://blog.hartwork.org/posts/expat-2-7-4-released/),
[oss-security, expat 2.9.0](https://www.openwall.com/lists/oss-security/2026/10/05/16),
[oss-security, expat 2.10.0](https://www.openwall.com/lists/oss-security/2026/11/02/6),
[Mageia advisory for 2.8.4](https://advisories.mageia.org/MGASA-2026-0378.html),
[Mozilla bug 1688452, sandbox libexpat using RLBox](https://bugzilla.mozilla.org/show_bug.cgi?id=1688452),
[Chrome: removing XSLT](https://developer.chrome.com/docs/web-platform/deprecating-xslt),
[blink-dev: XML parsing in Rust](https://groups.google.com/a/chromium.org/g/blink-dev/c/A-EM4e-JrTA).

## 11. Follow-up: PDF.js

### The trial

The browser was built at this commit with the default QuickJS engine.
`pdfjs-dist` 6.5.438 (1 November 2026, Apache-2.0), the current release, was
served from a local web server with a page that imports `build/pdf.mjs`,
starts the module worker `build/pdf.worker.mjs`, opens the 14-page
TraceMonkey paper that PDF.js uses as its own demo, and draws to a canvas.
The modern build was used, unmodified; the legacy build was not needed.

First result: the library loaded, the worker started and the document opened
(14 pages), but every page was blank and the worker logged `Unknown command`
for raw zlib data. Cause: an engine bug. `CompressionStream` and
`DecompressionStream` were a silent pass-through inside Workers, because the
zlib natives were bound only in window realms and the script fell back to an
identity transform. PDF.js inflates Flate streams with
`DecompressionStream`. The bug is fixed in the same change set as this
section (`ns_zlib_install` in `src/js.c`).

With that fix:

| Measure | Result |
|---------|--------|
| Library import | about 200 ms |
| Open document | 380–530 ms |
| Render one page at 795×1029 | 126–338 ms |
| All 14 pages | 3.9 s |
| Text extraction, page 1 | 182 items, correct |
| Visual check, page 1 | correct: embedded fonts, two-column layout, superscripts |
| Current poppler path, same file | about 2.3 s for all 14 pages, as images |

One document was tested. Not tested: JPEG 2000 and JBIG2 images (PDF.js
decodes these with WebAssembly modules, which would run under WAMR),
encrypted files, forms, CJK character maps, scanned documents, very large
files, memory use against the 512 MB worker limit, and the `pdf_viewer.mjs`
component with its text and annotation layers.

### What it would replace

`src/pdf.c` (172 lines) hands the bytes to poppler, renders each page to a
PNG, and builds an HTML page of `<img>` elements, capped at 300 pages. The
result has no selectable text, no search, no links and no zoom beyond image
scaling.

| | poppler path | PDF.js |
|---|--------------|--------|
| Parser of hostile bytes | about 4 MB of C++ in the renderer, plus its own dependencies | JavaScript inside the caged heap; image decoders in WebAssembly |
| Text selection, search, links | no | yes |
| Rendering | all pages up front | per page, on demand |
| Speed, this document | about 0.17 s per page | about 0.28 s per page |
| Size | system library | 0.47 MB display + 1.27 MB worker (minified), 0.42 MB WebAssembly decoders, 2.5 MB character maps and standard fonts |
| Availability | optional; absent builds show a notice | every build, every platform, including Android |
| Upkeep | distribution updates | each PDF.js release may need newer JavaScript features; version 6 uses `Map.prototype.getOrInsertComputed`, iterator helpers and `Intl.Segmenter` |

On security, PDF.js is the stronger design: a memory-safety bug in PDF
parsing becomes a bug in script the engine already has to contain. It has
its own history to respect: CVE-2024-4367 allowed script execution in the
viewer's origin through a crafted font. The viewer page must therefore have
no privileges, its own origin, a strict CSP, and `isEvalSupported: false`.

### Open questions for the owner

- **The Mozilla rule.** PDF.js is Mozilla code, though not engine code. It
  needs an explicit decision.
- **Shipping.** Committing 2–5 MB of third-party script sits badly with a
  tree meant to be readable by one person. The wgpu-native model fits
  better: a pinned, checksummed fetch at build time, never committed.
- **Licence.** Apache-2.0 is compatible with GPL-3.0-or-later. Its fit with
  NSL-1.0 needs checking, and the bundled OpenJPEG, JBIG2 and qcms modules
  carry their own licences.
- **No JIT.** Text documents are fine. Scanned and image-heavy documents are
  the likely weak spot and need measuring.

### Recommendation

Build a prototype behind a meson option, leaving the poppler path as it is:
an internal, unprivileged viewer page that uses the PDF.js display API with
lazy per-page rendering and the text layer, not Mozilla's full viewer UI.
Run it against a corpus (the PDF.js test PDFs are public) for correctness,
time and memory before deciding to switch the default. A side benefit is
already visible: PDF.js is a demanding conformance test for the engine, and
found a real bug in its first run.

Sources: [PDF.js releases](https://github.com/mozilla/pdf.js/releases),
[PDF.js FAQ](https://github.com/mozilla/pdf.js/wiki/Frequently-Asked-Questions).

