# Security review — November 2026

A source review of the first-party C code for security vulnerabilities,
done on 2026-11-10 against `main` at `ca746e7`. It was a **static**
review: nothing was built, run or fuzzed for it, so "confirmed" below
means the code path was traced by reading from attacker input to the
fault, not reproduced. Every finding was open when the review was
written; the Status column of the table below marks the ones fixed
since, and the fixing commits carry the slug as a `Security-Fix:`
trailer.

The threat model is the one in [SECURITY.md](../SECURITY.md): the
attacker controls everything fetched from the network, and a second
tier of findings assumes the attacker already has code execution inside
a sandboxed renderer.

## Summary

- **No attacker-reachable heap or stack corruption was found** in the
  parsers, decoders and typed-array bindings that were read. The
  memory-safety findings are two renderer crashes and a handful of
  robustness issues.
- **The most serious finding is a web-reachable policy bug:** a
  sandboxed `<iframe>` without `allow-same-origin` is opaque for storage
  and cookies but not for `fetch`, XHR and WebSocket, which run with the
  embedding site's origin and credentials (W1).
- Two more findings need no renderer compromise: the private-network
  guard is skipped for top-level `data:` documents (W2), and the address
  bar shows userinfo and percent-decodes the whole URL (W3).
- The shell/renderer IPC boundary held up well. The sandbox findings are
  about what a compromised renderer keeps at the operating-system level:
  cross-process signals and resource limits, SysV IPC, wide read access
  with open network, capture devices, and a shared writable profile.

| ID | Slug | Severity | Needs renderer compromise | Status |
|----|------|----------|---------------------------|--------|
| W1 | `iframe-sandbox-opaque-origin-not-applied-to-network` | High | no | **fixed** |
| W2 | `pna-bypass-via-opaque-top-level-document` | Medium | no | confirmed |
| W3 | `address-bar-userinfo-and-decoded-url-spoofing` | Medium | no | confirmed |
| W4 | `csp-unsupported-directives-fail-open` | Low | no | confirmed |
| W5 | `download-gate-gaps` | Low | partly | confirmed / plausible |
| S1 | `seccomp-cross-process-signals-and-limits` | Low–Medium | yes | confirmed |
| S2 | `seccomp-sysv-ipc-reachable` | Low–Medium | yes | confirmed (impact plausible) |
| S3 | `renderer-landlock-read-scope-plus-open-network` | Medium | yes | confirmed |
| S4 | `capture-devices-pregranted-to-every-renderer` | Low–Medium | yes | confirmed |
| S5 | `renderer-holds-display-connection-without-surfaceless-egl` | Medium, configuration-dependent | yes | confirmed (prevalence unknown) |
| S6 | `renderer-inherits-shell-environment` | Low | yes | confirmed |
| S7 | `shared-profile-lets-one-renderer-persist-into-other-sites` | Medium | yes | confirmed |
| S8 | `seccomp-best-effort-and-socket-fallback` | Low | yes | confirmed |
| S9 | `audio-helper-preopened-sound-server-connection` | Low | helper compromise | plausible |
| M1 | `unbounded-dom-recursion-svg-foreign-and-frame-images` | Low (DoS) | no | confirmed |
| M2 | `wasm-memory-cage-alloc-unchecked` | Low (DoS) | no | confirmed |
| M3 | `raw-svg-bytes-break-utf8-invariant` | Low | no | plausible |
| M4 | `webgpu-map-wait-stack-userdata` | Low | no | plausible |
| M5 | `zstd-final-flush-truncation` | Info | no | plausible |

The slugs are meant to be used as `Security-Fix:` trailers when the
fixes land.

## Web-reachable policy findings

### W1 — `iframe-sandbox-opaque-origin-not-applied-to-network` (High)

A frame sandboxed without `allow-same-origin` is given an opaque origin
only for storage and `document.cookie` (`src/js.c:15325`, `:15419`,
`:65046`). The network paths never consult
`ns_iframe_effective_sandbox`:

- `ns_js_fetch_base_url` (`src/js.c:10478`) and `st->origin_url = top`
  (`:10616`) take the fetch origin from the frame's real URL, or for
  `srcdoc` from the embedder's.
- `ns_fetch_omits_credentials` (`:9859`) and `cors_allows*` (`:9879`)
  therefore treat the request as same-origin with credentials.
- XHR uses `js->current_url` (`:22810`); WebSocket sends the site's real
  `Origin` (`:26136`).

So `<iframe sandbox="allow-scripts" srcdoc="…">`, or a sandboxed
same-site document, can `fetch('/account')` with the site's cookies and
read the response. Any site that relies on `sandbox` to host untrusted
HTML loses that guarantee.

Two related gaps share the cause:

- `NS_SANDBOX_ALLOW_POPUPS_ESCAPE` is parsed (`src/js.c:560`, `:576`)
  and never read, so a popup from a sandboxed frame with `allow-popups`
  opens unsandboxed with the real origin (`ns_popup_open`, `:27868`).
- `NS_SANDBOX_ALLOW_TOP_NAV` is consulted only for form submission
  (`:677`). `window.open(url, '_top')` reaches `nav_cb` unchecked
  (`:27880`). Top navigation through `top.location` and anchors is
  likely unchecked as well; that setter path was not traced end to end.

**Fixed.** Requests from an opaque realm carry an internal
`X-ND-Opaque-Origin` marker: the script side treats them as never
same-origin, the network layer sends `Origin: null`, preflights them and
marks them cross-site. Top navigation and popups from sandboxed frames
are gated on their flags; a popup without
`allow-popups-to-escape-sandbox` is blocked rather than opened
unsandboxed. Still open: subresource loads (`<img>`, stylesheets) from an
opaque frame are treated as same-site for cookies, though their
responses are not readable. The original recommendation follows.

**Fix.** Give each realm an explicit origin — tuple or opaque — fixed at
frame creation, with `sandbox` minus `allow-same-origin` yielding
opaque. Use it in `cors_allows*`, `ns_fetch_omits_credentials`, XHR, the
WebSocket and EventSource `Origin`, `ns_popup_realm_origin` and CSP
`'self'`. Send `Origin: null` and no credentials from opaque realms.
Propagate sandbox flags to popups unless
`allow-popups-to-escape-sandbox` is set, and check the top-navigation
flags in `nav_cb`, the location setters and `window.open`.

### W2 — `pna-bypass-via-opaque-top-level-document` (Medium)

`ns_net_address_guard_init` (`src/net.c:2540`) blocks a request to a
more private address space only when the document's own space is known.
`ns_address_space_of_document` (`:2345`) derives it from the origin, and
`ns_url_origin_from` (`:1186`) returns NULL for anything but http(s), so
a top-level `data:` document is `UNKNOWN` and nothing is blocked. The
WebSocket guard is keyed the same way (`src/ws.c:455`).

A public page can get there with `location = 'data:text/html,…'` or
`window.open('data:…')`; both are allowed
(`src/libnordstjernen.c:927`, `src/gtk/procwindow.c:2214`). The
resulting document can send image, form, `fetch` and WebSocket requests
to `127.0.0.1` and RFC 1918 hosts, and read the responses of targets
that answer `Access-Control-Allow-Origin: *` or accept `Origin: null`.

**Fix.** Have opaque documents inherit the initiator's address space
(the initiator is already known at `/open`,
`src/renderer_serve.c:861`), and treat `UNKNOWN` as public for
non-navigation requests. Consider refusing page-initiated top-level
`data:` navigation outright, as other browsers do.

### W3 — `address-bar-userinfo-and-decoded-url-spoofing` (Medium)

`address_display_url` (`src/gtk/procwindow.c:681`) is the only
processing a URL gets before it is shown.

- **Userinfo is shown verbatim.** `https://accounts.example.com@evil.test/`
  is displayed, and recorded in history, exactly like that. Nothing in
  the shell, `net.c` or `libnordstjernen.c` strips credentials.
- **The whole URL is percent-decoded.** `g_uri_unescape_string` runs
  over the full string and the result is rejected only for C0 controls
  and a few bidi marks. `%2F`, `%20`, U+3000, U+2044, U+FF0F and similar
  decode, so userinfo or a path can render as
  `https://bank.com/login      @evil.test/`.
- **The caret is placed at the end** (`set_address_text`, `:720`), so a
  long URL probably shows its tail rather than its origin. This depends
  on GtkText scrolling and was not verified.

**Fix.** Strip userinfo before display and before recording history.
Decode only path, query and fragment, never into delimiters or
whitespace, and keep non-printing and confusable characters encoded.
Show the URL from position 0, or emphasise the registrable domain.

### W4 — `csp-unsupported-directives-fail-open` (Low)

`src/csp.c:28-93` does not know `script-src-elem`, `script-src-attr`,
`style-src-elem`, `style-src-attr`, `manifest-src` or the `sandbox`
directive. A policy that restricts scripts only through
`script-src-elem` enforces nothing. Tokens are split on space and tab
only, so a `<meta>` policy with a newline right after the directive name
is ignored. No bypass was found in source matching, nonces, hashes or
`strict-dynamic`.

**Fix.** Map the `-elem` and `-attr` directives to their parents at
minimum, and split on all ASCII whitespace.

### W5 — `download-gate-gaps` (Low)

- Downloads are written to `~/Downloads` without a prompt unless the
  extension is on the executable list at `src/gtk/procwindow.c:1352`.
  The list misses, among others, `.scf`, `.jnlp`, `.application`,
  `.gadget`, `.msp`, `.psm1`, `.chm`, `.iso`, `.img`, `.vhd`, `.vhdx`,
  `.docm`, `.xlsm`, `.library-ms`, `.settingcontent-ms`, `.service`,
  `.csh`, `.ksh`, `.fish`, `.terminal` and `.workflow`.
- `download_move_posix` (`:1136`) rejects only symlinks after its
  `fstatat`. A compromised renderer could swap the staged file for a
  directory between the check and `renameat` (plausible, not traced
  under a race).

Name sanitisation, traversal handling and origin stamping are sound.

**Fix.** Extend the list, or prompt for anything outside a safe-type
allow-list. Open the staged file with `O_NOFOLLOW`, `fstat` the
descriptor and link or copy from it.

## Sandbox findings (renderer already compromised)

### S1 — `seccomp-cross-process-signals-and-limits` (Low–Medium)

SECURITY.md says a compromised renderer cannot signal the shell, other
renderers or any other process of the user. The allow-list in
`src/security.c:706-963` filters only `kill` and `tgkill` by pid
(`:1023`), and also allows:

- `rt_sigqueueinfo` (`:875`) and `rt_tgsigqueueinfo` (`:880`), which
  deliver any signal to any same-uid process;
- `fcntl` (`:746`), whose `F_SETOWN`/`F_SETSIG` do the same through
  `O_ASYNC`;
- `prlimit64` (`:849`), which can zero `RLIMIT_NOFILE` or `RLIMIT_AS`
  of the shell or any other process of the user;
- `sched_setaffinity`, `sched_setattr`, `sched_setscheduler`,
  `sched_setparam` (`:888-891`) and `setpriority` (`:906`) on other
  pids.

`LANDLOCK_SCOPE_SIGNAL` closes the signal paths on Landlock ABI 6
(kernel 6.12) and later only (`:490`); nothing covers `prlimit64` or
the scheduler calls.

**Fix.** Drop `rt_sigqueueinfo` or filter its first argument to the own
pid as for `kill`; filter `rt_tgsigqueueinfo` on tgid; restrict
`prlimit64`, `sched_set*` and `setpriority` to pid 0 or self; deny
`fcntl` with `F_SETOWN`, `F_SETOWN_EX` and `F_SETSIG`.

### S2 — `seccomp-sysv-ipc-reachable` (Low–Medium)

`semctl`/`semget`/`semop`/`semtimedop` (`src/security.c:894-897`) and
`shmat`/`shmctl`/`shmdt`/`shmget` (`:912-915`) are allowed without
argument filters, and `/proc` is readable (`:513`), so
`/proc/sysvipc/shm` lists the ids. Neither Landlock nor the filter
governs SysV IPC, so a compromised renderer can attach any same-uid
segment read-write. The concrete impact (MIT-SHM buffers of X11 clients,
other applications' shared state) was not demonstrated.

**Fix.** Remove the SysV calls from the renderer and audio-helper lists
— the framebuffer is a memfd — or allow `shmget` only with
`IPC_PRIVATE`.

### S3 — `renderer-landlock-read-scope-plus-open-network` (Medium)

The renderer's Landlock rules make `/proc`, `/sys`, `/run` and `/etc`
readable (`src/security.c:511-525`), plus all of `~/.config`,
`~/.local/share`, `~/.cache` and the XDG runtime directory
(`:554-558`). There are no Landlock network rules and `connect` is
unfiltered. A compromised renderer can therefore read and send out:

- other applications' secrets under the XDG directories — CLI tokens,
  cloud credentials, other browsers' cookie and login databases;
- everything under `/run`, including removable media at
  `/run/media/$USER`, GVFS mounts and container registry credentials.

It can also connect to any loopback or LAN TCP service, including the
unauthenticated `--webdriver` port when that is running: its `Host` and
`Origin` checks do not stop a raw client. SECURITY.md documents part of
this; the reach is wider than it suggests.

**Fix.** Replace the three XDG roots with the specific paths the engine
needs (fontconfig, its own directories), and `/run` with the specific
files needed. Add Landlock ABI 4 TCP rules, or move fetching to the
planned broker.

### S4 — `capture-devices-pregranted-to-every-renderer` (Low–Medium)

`src/security.c:527-537` adds `/dev/video0..63` and `/dev/snd`
read-write to every sandboxed process, and `ioctl` is unfiltered
(`:801`). The capability design has the shell decide camera and
microphone access, but at the OS level any renderer can open the devices
whatever `permissions.ini` says, so a compromised renderer can capture
silently for any site.

**Fix.** Leave capture devices out of the default ruleset. Have the
shell open the device after a grant and pass the descriptor over the
control socket, or respawn that tab's renderer with the device rule.

### S5 — `renderer-holds-display-connection-without-surfaceless-egl` (Medium, configuration-dependent)

`ns_gl_prepare_for_sandbox` runs between Landlock and seccomp
(`src/libnordstjernen.c:1202-1210`). When `EGL_MESA_platform_surfaceless`
is missing, `src/glctx.c:235-259` and `:380-386` fall back to
`eglGetDisplay(EGL_DEFAULT_DISPLAY)`, and the renderer keeps that
X11 or Wayland connection for its lifetime. On X11 that is a fully
trusted client — keystroke capture, input injection, screen capture —
which amounts to a sandbox escape. Which drivers lack the extension was
not checked.

**Fix.** Try `EGL_EXT_platform_device` or GBM next, and if neither
works disable WebGL in that renderer rather than connect to the user's
display. Show the state in `about:nordstjernen`.

### S6 — `renderer-inherits-shell-environment` (Low)

The renderer is started with `execv` and an untouched environment
(`src/rproc_http.c:200-218`), as is the audio helper
(`src/gtk/procview.c:950`). A compromised renderer can read API tokens,
`SSH_AUTH_SOCK`, `DBUS_SESSION_BUS_ADDRESS` and `XAUTHORITY` from its
own memory with no filesystem access. Terminal injection through the
inherited tty is already blocked (`src/security.c:1030-1046`).

**Fix.** Use `execve` with an allow-listed environment (locale, `HOME`,
`XDG_*`, fontconfig, `NS_*`, proxy and CA variables), and point
descriptors 0–2 at `/dev/null` unless debugging.

### S7 — `shared-profile-lets-one-renderer-persist-into-other-sites` (Medium)

Largely acknowledged in `docs/tab-isolation.md`, but two concrete paths
are worth recording.

- **Disk cache.** Every renderer has read-write access to
  `~/.local/share/nordstjernen` and `~/.cache/nordstjernen`
  (`src/security.c:573-600`). `docs/Software-Architecture.md` explains
  that the bytecode cache is memory-only because a shared on-disk cache
  would let one compromised renderer plant code that others run. The
  on-disk HTTP cache (`src/cache.c:188-209`, `:385-389`) has exactly
  that property: a compromised renderer can rewrite another site's
  cached script, and the change survives a restart. It can also read
  every site's cookies, localStorage and IndexedDB.
- **Popup relay.** The shell forwards the popup `call` operation with
  the `origin` field the sending renderer supplied
  (`src/gtk/procwindow.c:2164-2184`, `:2288`). The opener checks only
  that the string equals its own origin (`src/js.c:28150`) and then
  calls an arbitrary function path in its main realm. A compromised
  popup renderer whose opener is another site can thus run script in
  that site's renderer.

**Fix.** For the cache, authenticate each entry with a key the shell
holds or serve it through a shell broker; short term, give each renderer
a per-site cache directory. For the relay, have the shell stamp `origin`
from its own record of the sender tab's committed URL and drop `call`
unless it matches the origin recorded at `open`.

### S8 — `seccomp-best-effort-and-socket-fallback` (Low)

A failed seccomp load only warns unless `NS_REQUIRE_SANDBOX` is set
(`src/security.c:1095-1100`); if any argument-filtered `socket` rule
fails to add, `socket` is allowed for every domain (`:1012-1022`); and
every `seccomp_rule_add` result is discarded. Without the filter the
renderer keeps `execve` and `AF_UNIX`, so the session D-Bus is
reachable. Landlock failure is already fatal.

**Fix.** Make seccomp failure fatal in the renderer and audio helper by
default, with an opt-out, and deny rather than allow all when a socket
rule cannot be added.

### S9 — `audio-helper-preopened-sound-server-connection` (Low, plausible)

The audio helper opens its SDL device before `sandbox_self`
(`src/audio/main.c:1911-1931`), so it keeps a PulseAudio or PipeWire
connection after new `AF_UNIX` sockets are denied. A helper compromised
through a decoder might use it for microphone capture or to load server
modules; the server side was not examined.

**Fix.** Document it as residual risk at minimum. Ideally the shell owns
audio output and the helper only decodes to PCM.

## Memory-safety findings

### M1 — `unbounded-dom-recursion-svg-foreign-and-frame-images` (Low)

DOM depth is not capped at insertion, and nearly every tree walker
guards with `NS_DOM_MAX_DEPTH`. Two do not:

- `collect_svg_foreign_objects` (`src/layout.c:5936`) descends every
  element under an inline `<svg>`, with or without a style entry;
- `ns_js_collect_frame_images` (`src/js.c:71860`) walks an iframe
  document on every iframe load.

A document or script that nests on the order of 10^5 elements under an
inline `<svg>`, or inside an iframe, exhausts the stack and crashes the
renderer. The guard page makes it a clean fault.

**Fix.** Add a depth parameter with the `NS_DOM_MAX_DEPTH` guard the
neighbouring walkers use, or make both iterative.

### M2 — `wasm-memory-cage-alloc-unchecked` (Low)

`ns_js_cage_shared_malloc` and `ns_js_cage_shared_realloc` return NULL
when the 4 GB cage is exhausted. `ns_wasm_memory_ctor`
(`src/wasm.c:2258`) passes the result straight to `memset`.
`ns_wasm_memory_grow` (`:803`) overwrites `m->staging` with the realloc
result and then zero-fills from `m->staging + m->staging_size`, which
with NULL is a write starting at an address between 64 KiB and 512 MiB.
Script reaches both by filling the cage with buffers first. The old
block is leaked rather than freed, so there is no use-after-free.

**Fix.** Check both results. Throw `RangeError` from the constructor; in
`grow`, realloc into a temporary and leave `m->staging` untouched on
failure.

### M3 — `raw-svg-bytes-break-utf8-invariant` (Low, plausible)

Main documents go through `ns_html_decode_body_full` and are always
valid UTF-8. SVG-as-image (`src/svg.c:3843`, `:3891`) and external
`<use>` (`src/svg_external.c:96`) pass raw network bytes to
`ns_html_parse`. Several walkers step with `g_utf8_next_char` without a
length clamp — `ns_dir_first_strong` (`src/css.c:25447`, `:25476`),
`parse_legacy_color` (`:33281`), `ns_vertical_stack_text`
(`src/layout.c:1668`), `ns_utf16_length` (`src/js.c:7652`) — so a text
node or attribute ending in a truncated lead byte makes them step over
the terminator and read adjacent heap until a zero byte. The result
feeds only a boolean or a colour. Not verified: that lexbor passes
invalid bytes through unchanged.

**Fix.** Validate, or run `g_utf8_make_valid` over, the bytes at those
three call sites, and clamp the listed walkers as `src/dom.c:2461` does.

### M4 — `webgpu-map-wait-stack-userdata` (Low, plausible)

`wg_buffer_mapAsync` (`src/webgpu.c:853`) and
`ns_webgpu_canvas_surface` (`:4991`) hand a stack-local `wg_map_wait`
to wgpu-native as callback userdata, poll at most 4000 times, and return
without cancelling if it has not fired. A callback delivered on a later
`wgpuInstanceProcessEvents` would write through a dead stack frame. The
same shape is at `:615`, `:1191`, `:1288`, `:3726` and `:3757`. Whether
wgpu-native can leave a request pending that long was not established.

**Fix.** Heap-allocate the wait record and free it from the callback,
or on timeout unmap and keep pumping until the callback has fired.

### M5 — `zstd-final-flush-truncation` (Info, plausible)

The zstd case of `stage_feed` in `src/content_decode.c` loops while
`in.pos < in.size` with a 16 KiB output buffer. `ZSTD_decompressStream`
can consume all input while still holding output, and the decoder has no
finish step, so the tail of a body can be dropped. No memory impact.

**Fix.** Loop while `in.pos < in.size || o.pos == o.size`.

### Hardening notes

- `src/video_decode.c:473-488`, `:550-568`: the BGRA buffer comes from
  `av_malloc` and the `sws_scale` return value is ignored. A short
  conversion would leave uninitialised heap in a texture that script can
  read back through a canvas. Use `av_mallocz` or reject a short return.
- `src/svg.c:2015-2022`: `cairo_image_surface_get_data` is dereferenced
  without re-checking the surface status after rendering.
- `src/css.c:5464`: author-controlled grid line names go through
  `g_intern_string`, which never frees, so script cycling unique names
  grows memory without bound.
- `src/js.c:24183`: `JS_GetArrayBuffer` on the TextDecoder `_tail`
  leaves a pending exception when the value is not an ArrayBuffer.

## Documentation drift in SECURITY.md

- It names `http` and `https` as the only protocols; `src/net.c:2025`
  also enables `ftp` when libcurl has it (top-level navigations only;
  subresources from non-FTP pages are refused at `:7076`).
- It still describes a Netscape cookie jar; the store is SQLite
  (`src/cookies.c`).
- It says the audio helper keeps `AF_UNIX`; the code denies it
  (`src/audio/main.c:1893`).
- Its list of unprompted config keys includes `gsk_renderer`; the code
  (`src/config.c:498`) is stricter.

Smaller observations:

- `ns_net_url_is_chrome_page` (`src/net.c:5862`) treats every `about:`
  name except `blank` and `srcdoc` as a privileged page, unknown names
  included. No way to put content into such a page was found; an
  explicit allow-list would be safer.
- `src/history.c:83` migrates a legacy `history.sqlite` out of the
  renderer-writable data directory when the state copy is missing.
- A compromised renderer can set the clipboard
  (`src/gtk/procview.c:3862`) and clear history through
  `X-History-Clear` (`src/gtk/procwindow.c:1863`) without confirmation.
  Web content cannot.

## Reviewed and found sound

**Process boundary**

- The IPC transport is an `AF_UNIX` socketpair on descriptor 3 with
  `close_range` in the child (`src/rproc_http.c:187-218`): no TCP or
  named socket for local processes or web pages to reach. The
  framebuffer memfd is sealed, and frame geometry is validated against
  the mapping before use (`:768-786`).
- IPC parsing uses bounded header buffers, a header-count cap and
  clamped numbers (`src/ipc_http.c:258-423`).
- The side channels are tightly scoped: `X-Audio` to an operation and
  path allow-list, `X-Config` to a diff of the parsed struct with a
  prompt outside seven keys, `X-Download` and `X-Popup` to scheme
  allow-lists, `X-Window-Action` to two fixed strings.
- No renderer-controlled argv, no `system`, `popen` or shell strings.
- Shell state (`permissions.ini`, bookmarks, session, config, history,
  the control token) sits where renderers have no write access.
- The seccomp list has no `execve`, `ptrace`, `bpf`, `io_uring`,
  `userfaultfd`, `mount`, `unshare`, `setns`, `listen`, `accept`,
  `pidfd_getfd` or `process_vm_*`; `clone3` returns `ENOSYS` and
  namespace flags are masked.

**Network and web policy**

- TLS verification is on everywhere; the opt-in override is top-level
  only, never for HSTS hosts, and sends no cookies. Redirect protocols
  are clamped.
- Privileged `about:` and `view-source:` pages are gated twice, state
  changes require POST, and history HTML is escaped.
- `file:` is blocked from remote pages and confined to the page's own
  directory for local ones. `javascript:` URLs run only on anchor
  activation in the anchor's realm, under CSP and sandbox checks.
- Cookies: public-suffix check, host-only versus domain, `Secure` from
  insecure origins, `__Host-`/`__Secure-` prefixes, `HttpOnly` neither
  readable nor overwritable from script, SameSite defaulting to Lax.
- Forbidden header names and CR/LF/NUL are rejected for fetch headers
  and WebSocket subprotocols.
- SQL uses bound parameters only; `ns_db_harden` sets defensive mode and
  disables extensions. Cache file names are SHA-256 keys.
- Remote control is off by default, loopback only, with a 256-bit token,
  constant-time compare and a 0600 token file. The WebDriver transport
  binds loopback, allow-lists `Host` and refuses `Origin`.
- Internal `data-nd-*` attributes are filtered from parsers,
  `setAttribute`, attribute nodes and `dataset`.

**Parsers and bindings**

- `mp4.c`, `video_mp4.c`, `media_os.c`, `media_os_gst.c`,
  `video_decode.c`, the MSE path of `video.c`, and `audio/main.c`: box,
  table and sample reads are bounded, counts and dimensions capped,
  growth loops overflow-checked.
- `image.c`, `image_ico.c`, `image_wuffs.c`, `image_webp.c`,
  `image_avif.c`, `image_jxl.c`, `texture.c`: 16384-per-side and
  64M-pixel caps, 64-bit size arithmetic, stride × height checked
  against the buffer.
- `woff2.c` and the WOFF rebuild in `font.c`: bounded stream reads,
  bounded counts, output caps.
- `content_decode.c` (apart from M5), `json_string.c`,
  `bytecode_cache.c`, `pdf.c`, `xml.c`, and the on-disk parsing in
  `cache.c`.
- `js_canvas.c`, `js_canvas_api.c`: backing-store pointers are fetched
  after all script-visible conversions and lengths checked against
  `w*h*4`; canvas dimensions capped at 8192.
- `webgl.c`: unpack parameters, index and attribute ranges, `readPixels`
  and compressed uploads are length-checked with overflow checks.
- `wasm.c` (apart from M2), `webcrypto.c`, `webaudio.c`.
- In `js.c`, all 41 ArrayBuffer and typed-array access sites validate
  offset and length against the live buffer and use the pointer before
  further script can run; removed nodes are parked until teardown and
  their wrappers invalidated.
- Grid placement in `layout.c`, and the fixed buffers and depth caps
  checked in `css.c`, `css_shape.c`, `css_anchor.c`, `css_color.c`,
  `svg.c`, `paint.c` and `dom.c`.

## Not covered

This review did not reach everything, and the gaps matter as much as the
findings.

- The vendored engines — QuickJS, lexbor, WAMR, web-text-lib, Wuffs,
  pl_mpeg, minimp3 — were not audited, only how first-party code calls
  them.
- `src/js.c` (73,000 lines) was read through targeted searches only.
  Not systematically reviewed: event dispatch and listener mutation,
  MutationObserver and custom-element reentrancy, Blob and File slicing,
  Range and Selection, workers and message ports, iframe realm teardown,
  in-tab `postMessage` origin checks, localStorage and IndexedDB origin
  keying, service-worker scope, BroadcastChannel, the cross-origin
  `WindowProxy` property set, and whether every callback entry point
  enters the right realm scope.
- `layout.c`, `paint.c` and `css.c` were reviewed for fixed arrays,
  grid, recursion and filters. Tables, flex, floats, inline layout, the
  selector matcher and animation interpolation were not read line by
  line.
- Not reviewed: `media_os_mf.c`, `media_os_vt.c`, `media_os_android.c`,
  `idb.c`, `camera.c`, `ext.c`, `safebrowsing*.c`, `eventsource.c`,
  `watchdog.c`, `security_root.c`, `capability.c`, `proc_limits.h`,
  `headless.c`, the embedder API of `libnordstjernen.c`, the WebDriver
  and MCP command handlers, WebAuthn, the Rust IPC variant, and the
  macOS Seatbelt and Windows mitigation paths.
- `webgpu.c` beyond buffer mapping, canvas configuration and readback.
- The per-origin CSP table in `net.c` (`g_csp_by_origin`), which may let
  one same-origin document's policy replace another's.
- Nothing was run under a sanitizer or fuzzer. The parsers that read as
  sound here are the natural next targets for that.
