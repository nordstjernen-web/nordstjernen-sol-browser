# Security review report, 2026-11-21

Source review of Nordstjernen at commit `983d178b`, looking for security
vulnerabilities in the first-party source. It follows the reviews of
2026-11-13 and 2026-11-18, whose reports are no longer in the tree, and was
aimed first at the areas they did not reach. The F, S, G, N and J item
numbers in §5 are theirs. Two parallel reviewers did the reading: one on
memory safety, one on web policy, network and the process boundary.

The review itself changed no source file. M1 and C1 were fixed afterwards on
request, and C2 and M2 after that; nothing else is fixed.

## 1. Summary

- **One critical bug: a universal same-origin-policy bypass (C1).** Any
  framed document, including an opaque `data:` frame, gets the embedder's
  real global object from an exception thrown across the cross-window proxy.
  It then reads and writes the embedder's DOM. Reproduced, then fixed.
- **One high-severity memory bug: a Web Audio out-of-bounds read (M1).** Any
  page can trigger it with no interaction. Fixed (`1f012fa8`) and the fix
  was run.
- **Three medium WebGL findings (M2–M4; M2 fixed):** uninitialised GPU storage handed
  to the page, and two gaps in draw-call bounds validation.
- **One medium policy finding (C2):** a hostname that starts with `127.` is
  treated as loopback, so `http://127.example.com/` counts as a secure
  context and loses the "Not secure" chip.
- **J15 from the earlier review is upgraded** from "unknown" to a medium
  use-after-free read, traced further but not run.
- **J5 is fixed.** Every other earlier open item checked is still open (§5).
- No new memory-corruption bug was found in the regexp capture widening
  (`18d8fc40`), the cage allocator, table layout, the blur buffers, MSE
  segment bookkeeping, WebCrypto or WebSocket framing.

| # | Finding | Attacker | Severity | Evidence |
|---|---------|----------|----------|----------|
| C1 | Exception thrown in the target realm leaks that realm to a cross-origin caller | any framed or popup document | critical | run on the headless browser; fixed |
| M1 | Web Audio `_evn` wraps the bounds check; out-of-bounds read returned to script | any web page | high | run; fixed in `1f012fa8` |
| M2 | WebGL hands out GL storage that was never initialised | any web page | medium | run; fixed |
| M3 | WebGL2 index validation uses a stale shadow of the element buffer | any web page | medium (high on non-robust contexts) | traced in source; not run |
| M4 | WebGL buffer sizes tracked by GL name; deleted buffer still attached to a vertex array | any web page | low–medium | plausible; needs driver name reuse |
| C2 | `127.`-prefixed hostnames treated as loopback in three places | any web page on such a host | medium | run; fixed |
| J15 | Style value pointer used after a layout flush that can free it | any web page | medium | traced further; not run |

"Run" means confirmed against a build in this tree, in private mode, on local
scratch pages. M2–M4, C2 and J15 are as the reviewers reported them; only C1
and M1 were re-checked when the reports were merged.

## 2. Method and coverage

Each reviewer was told to trace every finding from attacker-controlled input
to the faulty operation, to drop anything it could not trace, and to stay
read-only. No sanitizer build was made and no fuzzer was run.

### Memory-safety slice

| Area | Read closely | Grep-driven or partial | Not reached |
|------|--------------|------------------------|-------------|
| CSS | In `css.c`: token-array call sites of the split helpers; `contain-intrinsic`, grid-line, transform-argument, gradient-stop and `attr()` buffers; inline-style slot caches and the declaration-sheet cache; `ns_style_free`, `ns_css_value_free`; the two rollback functions | Depth guards across `css.c`, `css_anchor.c`, `css_media.c`, `css_color.c` | Selector parsing and matching bodies, cascade gather, `var()` substitution, `calc()` evaluation, `@`-rule parsing, incremental restyle; `css_filter.c`, `css_counter_style.c`, `css_grammar.c`, `css_syntax.c`, `css_prop_syntax.c`, `css_shape.c`, `css_sizes.c` beyond the depth grep |
| Layout | Table layout (`layout.c:10590-10720`, `11880-11925`, `12060-12480`), counter and marker buffers | Allocation and fixed-array sites; depth caps on box building | Flex, grid, multicol, float and inline layout bodies; hit testing |
| Paint / SVG | `ns_box_blur_a8`, `box_blur_argb*`, `ns_paint_blur_argb`, list-marker text; `svg_mask_surface`, `svg_filter_blur`, `svg_blur_source` | Surface and allocation sites in `paint.c` and `svg.c`; SVG depth, node and pixel caps | Most of `paint.c`; SVG path parser, gradients, patterns, markers, text, `<use>`; `render.c`, `anim.c`, `smil.c`, `mathml.c` beyond an allocation grep |
| DOM / forms / selection | — | `ns_invalidate_wrapper`, orphan sweep, depth caps in `dom.c`; `selection.c` byte clamps | `forms.c`; `dom.c` bodies; the `html_parser.c` sink (read in the earlier review, not re-read) |
| WebGL | `webgl.c` 676-1245, 1480-1740, 1837-1961, 2440-2545, 2632-3290, 3312-3950, 3978-4240, 4325-4460, 4527-4600, 4636-4790, 4846-5430; context lifetime 268-560; `glctx.c` robustness paths | Extension table, remaining getters | Readback and PBO streaming (6333-6570), constants and binding tables |
| WebGPU | Buffer map / unmap / `getMappedRange`, `writeBuffer`, `writeTexture`, `copyExternalImageToTexture`, canvas context configure / target / readback, dynamic offsets, view formats, constants | Handle unwrap pattern | Pipeline, bind-group and pass descriptor readers; error scopes; device loss |
| Web Audio | All of `webaudio.c` and the `js.c` helpers it depends on | — | — |
| Media | `video.c` MSE unit splitting (296-468) and append path (1716-1826) | Chunk and window bookkeeping | Playback tick, audio materialisation, seeking, VTT |
| Other bindings | `webcrypto.c` AES, ChaCha, ECDSA, ECDH, PBKDF2, HKDF, raw import; `ws.c` frame handling | `idb.c`, `eventsource.c`, `content_decode.c`; `wasm.c` memory-buffer paths | `js_canvas.c`, `js_canvas_api.c`, `xml.c` (read in the earlier review, not re-read) |
| Engine | The `18d8fc40` diff in full and every capture-count use in `libregexp.c` and `quickjs.c`; `js_cage.c`; `quickjs-cage.c` 296-746; `quickjs-cptr.h` | `quickjs-hardening.h` (declarations); `bytecode_cache.c` (persistence check) | `ns_quickjs.c`; remaining fork changes in `quickjs.c` |

### Policy and boundary slice

| Area | Read closely | Grep-driven or partial | Not reached |
|------|--------------|------------------------|-------------|
| Same-origin policy, frames (`js.c`) | Cross-window and iframe bootstrap (56189-58415), `postMessage` (16690-16971, 29318-29329), realm scope enter / leave (1977-2470), frame realm create / replace / retire (57876-58060, 75015-75300), `ns_js_load_iframe_in_embedder` (76294-76972), engine-private names, the native bindings table (65560-66380), `js_webauthn.c`, the rp-id check in `gtk/ns_webauthn.c` | `js_realm.c`, `js_brand.c`, storage events, BroadcastChannel, worker and service-worker CSP | Custom-element upgrade, import-map internals, most of `js.c` 62250-63940 |
| Fetch, CORS, navigation (`net.c`) | `ns_hop_transport` with TLS, redirect and certificate-override logic (6219-6454); partition, origin, CSP and allowlist registries (6592-6900); `file:`, `about:`, `view-source:` and `data:` synthesis (4366-6000); `ns_file_access_allowed`; cross-origin header stripping (7612-7660); curl options; origin logic in `url_rs/bytes.rs` | `ws.c`, `eventsource.c`, HSTS and alt-svc, DoH, private-network guard internals | `netutil.c`, mixed-content upgrade edge cases |
| Cookies, storage, cache | `cookies.c` 620-1150 | `cache.c`, `idb.c`, `db.c`, `bytecode_cache.c` | Cache path construction |
| CSP, about pages, extensions, PDF | `csp.c` source matching, `about_page.c`, the `history.c` page builders, `ext.c` storage paths, `ns_ext_safe_path` and content-script injection, privileged-page gating (`net.c:5623-5665`) | `permissions_policy.c`, `connection_allowlist.c`, `safebrowsing*.c`, `security.c`, `pdf.c` | — |
| Process boundary, shell | `procview.c` result handler and side-channel consumers (1325-1470, 3840-4090), audio pump (800-1060), `procwindow.c` address display (660-860), `remotectl.c` token handling, WebDriver listen and host checks, the listening-socket inventory | `rproc_http.c`, `renderer_serve.c`, `ipc_http.c` header parsing, `mcp.c`, `rjson.c`, `pagelayers.c` | macOS Seatbelt at runtime, Windows token and job specifics, `audio/main.c` parser internals, session-restore parsing |

**Limits.** The CSS cascade, selector matching, `var()` / `calc()`, flex, grid
and inline layout and most of `paint.c` were again not read; three reviews
have now skipped them. The seccomp filter contents, download filename
handling, UI-spoofing surfaces other than the address bar, and the build and
supply-chain items were in the brief but do not appear in the reviewer's
coverage, so treat them as not reviewed.

## 3. Findings

### C1. A cross-realm exception leaks the victim's global object (critical, fixed)

**Where.**
- Throw sites that run in the *target* window's realm:
  `src/js.c:16837-16838` (`postMessage` with an unparsable target origin, in
  `ns_post_message_to_target_in`), `src/js.c:29329` (the same throw in the
  opener / popup relay), and `src/js.c:69437-69447` (`ns_location_set_href`
  with an invalid URL).
- The bridges that carry a cross-origin caller into that realm: the iframe
  proxy at `src/js.c:56479-56482` calls the parent's `__nsPostMessageFrom`;
  its `set` trap at `:56490` assigns `target.location.href`; the opener
  proxy does the same at `src/js.c:58263-58270` and `:58299-58301`.

**Why it is wrong.** The cross-window proxy denies every property, but the
native it forwards to creates its exception in the realm it belongs to, which
is the victim's. The exception propagates out through the proxy into the
caller's `catch`. From any object of a realm, `e.constructor.constructor` is
that realm's `Function`, and `Function('return this')()` is its global.

**Attack.** No user interaction; the attacker only has to be framed by, or
hold a window reference to, the victim:

```html
<title>VICTIM-TITLE</title><p id=sec>SECRET</p>
<iframe src="data:text/html,<script>
  try { parent.postMessage('x', 'nonsense') }
  catch (e) {
    var g = e.constructor.constructor('return this')();
    g.document.getElementById('sec').textContent = 'PWNED:' + g.document.title;
  }
</script>"></iframe>
```

**Evidence.** Run on `builddir/src/gtk/nordstjernen.exe --headless
--dump=text` in private mode: the dump is `PWNED:VICTIM-TITLE`. The frame is
an opaque-origin `data:` document and the embedder a `file:` page. The
reviewer also confirmed that `parent.document` from the same frame throws
`SecurityError`, so the proxy itself holds; only the exception path leaks.
The two-different-http-origins case was not run, but the path does not
depend on the origins involved. With the global in hand the attacker has the
victim's DOM, `document.cookie` and credentialed `fetch`.

A wider probe showed the same leak from `DataCloneError` (posting a
function, a symbol or a bad transfer list), from `location.href` and
`location = ...` with an invalid URL, through `top`, and through
`event.source`.

**Fix applied.** Both proxy builders now route every call into the target
window (`postMessage`, `location.href`, `location.replace`, `closed`,
`length`, `close`, `focus`, `blur`) through `crossCall`, which catches what
the target throws. An error that does not inherit from the caller realm's
`Object.prototype` is replaced by a new error of the caller's realm carrying
only the name and message strings; the caller's own errors (a getter that
throws during cloning) pass through unchanged. Location values are converted
to strings in the caller's realm first. `ns_post_message_to_target_in`
throws its argument-count and target-origin errors in the caller's realm.
After the fix every probe that throws gets an error of the frame's own realm,
the page above prints `SECRET`, and messages still arrive in both directions.

Northstar and Southstar share this proxy code, so this needs a
`Security-Fix:` trailer and a port.

### M1. Web Audio: `_evn` wraps the bounds check (high, fixed)

**Where.** `src/webaudio.c`, `ns_wa_param_sync`, `ns_wa_param_at`,
`ns_wa_param_cancel`, `ns_wa_param_eval`; `ns_audio_make_param` in `src/js.c`.

**Why it was wrong.** An AudioParam is a plain object whose automation state
is the page-writable properties `_ev` (a Float64Array of four-value events)
and `_evn` (the count). The automation methods have no brand check, so `this`
can be any object. Both users guarded with `n * 4 <= cap` on a `uint32_t`;
`_evn = 0x40000000` makes the product zero. `ns_wa_param_eval` then walked
`n` events without looking at `cap`, and the value it reached went back to
script as a held event or as `param.value`. With `prune` set the same wrap
led to a `memmove` of about 32 GB, which crashes the renderer.

**Trigger.**

```js
const g = new OfflineAudioContext(1, 128, 44100).createGain();
const p = { value: 0, _base: 0, _evn: 0x40000000,
            _ev: new Float64Array(4).fill(-1) };
g.gain.cancelAndHoldAtTime.call(p, 0);
```

**Fix applied.** `ns_wa_param_sync` clamps the count to `NS_WA_MAX_EVENTS`
(4096) and rejects NaN and negative values before the integer conversion;
the three capacity checks compare `n <= cap / 4`. On the patched build the
trigger leaves `_evn = 0` and the array untouched. An unpatched build from
the same day (`builddir-ci`) accepted the count and inserted a held event.
The read stays inside the JS cage, so what leaks is other JavaScript heap
data from the same process, including other frames' realms.

**Still worth doing.** Keep automation state in a C struct behind a class id
and brand-check `this`, so the page cannot write it at all.

### M2. WebGL hands the page GL storage that was never initialised (medium, fixed)

**Where and why.**
- **Drawing buffer after a resize.** `ns_webgl_sync_size`
  (`src/webgl.c:333-350`) and `wgl_drawingBufferStorage` (`:5407-5429`)
  reallocate through `ns_webgl_alloc_storage` (`:268-316`) with no clear.
  Only `ns_webgl_make` clears (`:441-443`), and only colour.
- **`texStorage2D` / `texStorage3D`** (`:4325-4332`, `:4779-4786`) allocate
  with no zero fill; the `texImage2D(..., null)` path does fill
  (`:3561-3563`).
- **Renderbuffers.** `wgl_renderbufferStorage` (`:3876-3884`) only marks the
  buffer uncleared. The clear runs inside `wgl_framebufferRenderbuffer`
  (`:3821-3854`), so attaching first and allocating afterwards never clears.
- **`bufferData(target, size)`** (`:2851-2859`) passes no data except for
  element arrays, and WebGL2 `getBufferSubData` (`:4404-4423`) copies the
  bytes to script.

**Impact.** Stale GPU or driver memory, which can hold pixels from other
WebGL contexts or video textures in the same renderer. How much leaks
depends on the driver; none was exercised.

**Evidence.** Run on a Windows driver without robust contexts, after
filling and freeing textures and buffers with a pattern. Before the fix a
1 MiB `bufferData(size)` buffer read back with every byte non-zero, as did a
4096-byte uniform buffer; mip levels 3 and 6 of a `texStorage2D` texture, a
renderbuffer allocated after being attached, a re-allocated renderbuffer and
a resized antialiased WebGL 1 canvas all returned stale pixels.

**Fix applied.**
- `ns_webgl_alloc_storage` clears colour, depth and stencil of the drawing
  buffer, and of the resolve target of an antialiased one, every time.
- `bufferData(size)` uploads zeros for every target.
- `texStorage2D` / `texStorage3D` upload zeros to every level, face and
  layer, in bands of at most 4 MiB, and only when the call really made the
  texture immutable. A format that is neither a known sized format nor a
  known compressed one is refused with `INVALID_ENUM`.
- `renderbufferStorage` and `renderbufferStorageMultisample` clear the new
  storage at once through a scratch framebuffer, which replaces the
  clear-on-attach bookkeeping. Integer colour formats are cleared with the
  integer clear calls.

After the fix every read in the same test returns zeros, twenty sized
formats and a compressed one allocate without a GL error, uploads after
`texStorage2D` still work, and a second `texStorage2D` on the same texture
still fails without wiping it.

### M3. WebGL2 index validation uses a stale element-buffer shadow (medium)

**Where.** `src/webgl.c:2714-2722`, `3225-3241`, `3897-3907`, `4846-4889`.

**Why it is wrong.** `wgl_draw_elements_ok` takes the maximum index from a
CPU copy of the element buffer. Two GPU-side writes leave that copy stale:

1. `readPixels` into a pixel pack buffer (`:3899-3907`) does not drop the
   copy, and `wgl_bindBuffer` does not latch a buffer's type, so an element
   buffer can be bound to `PIXEL_PACK_BUFFER`.
2. Transform feedback writes the buffer; after `pauseTransformFeedback()`
   and a switch to another feedback object the stale copy is used again.
   Only `endTransformFeedback` drops it (`:4868`).

The draw is then validated against old indices and executed with new ones.
On a robust-access context the driver clamps the fetch. On Windows drivers
without `WGL_ARB_create_context_robustness`, `src/glctx.c:128-141` falls
back to an ordinary context and relies on these checks, which makes this
high there.

**Fix.** Latch buffer type as WebGL requires; drop the shadow of any buffer
written through `PIXEL_PACK_BUFFER` or a feedback binding; refuse WebGL2 on
a non-robust context.

### M4. WebGL buffer sizes are tracked by GL name (low–medium, plausible)

**Where.** `src/webgl.c:2689-2711`, `979-1030`, `3187-3200`.

`wgl_deleteBuffer` drops the size and shadow entries for a name and deletes
the buffer. GL keeps the object alive while a non-current vertex array still
references it. If the driver gives the same name to a new, larger buffer,
`wgl_attribs_cover` and `wgl_elements_in_range` check the new buffer's size
while GL draws from the old one. The tracking gap is traced; name reuse by
the driver is assumed. Fix: key the tracking on the wrapper object, or keep
the entry while any vertex array references the buffer.

### C2. `127.`-prefixed hostnames are treated as loopback (medium, fixed)

**Where.** `src/js.c:32809`, `src/js.c:62948`, `src/gtk/procwindow.c:813`,
each `g_str_has_prefix(host, "127.")`.

The prefix matches the DNS name `127.example.com` as well as the
`127.0.0.0/8` literals. A page at `http://127.example.com/` is therefore a
trustworthy origin: it may register a service worker (`src/js.c:32860`),
passes secure-context gating, and the shell shows it without the "Not
secure" chip (`procwindow.c:858`). The earlier J17 named only the first
site.

**Fix applied.** `ns_host_is_loopback` moved to `src/netutil.c`, shared by
the engine and the shell, and now parses the host as an address literal:
`localhost`, `*.localhost`, `127.0.0.0/8` and `::1` qualify. All three sites
call it. Run through a local proxy: `http://127.example.com/` registered a
service worker before and is refused with `SecurityError` after;
`127.0.0.2` and `localhost` still register.

### J15, upgraded: style value used after a layout flush (medium)

In the computed-style lookup, `sv` is taken from `js->style_table`,
`ns_js_flush_layout(js)` runs at `src/js.c:20588`, and `sv->u.keyword` is
read at `:20595`. The flush reaches `browser_relayout`, which destroys both
style tables (`src/libnordstjernen.c:288-291`) when layout is dirty, and it
is dirty for properties outside `ns_computed_needs_layout`
(`src/js.c:19308-19324`), such as `max-width: anchor-size(...)`. Values are
refcounted, so `sv` is freed only when the style was its last owner; the
reviewer did not trace whether the inline-declaration cache
(`src/css.c:27346-27377`) keeps a second reference. Fix: look the style up
again after the flush.

## 4. Observations not established as findings

- **Internal natives still on `window`.** Not deleted by
  `data/js/polyfills.js`: `__ns_set_highlights`, `__ns_set_interest_state`,
  `__ns_linked_css`, `__ns_css_supported`, `__ns_cssom_pending`,
  `__ns_container_query_canonical`, `__ns_property_rule_valid`,
  `__ns_anim_list` / `_query` / `_seek` / `_control` / `_animate` /
  `_keyframes` / `_base_value`, `__ndMediaListSerialize`, `__ndMediaBySrc`,
  `__ndMse*`, `__ndNativeRange`, `__ndCreateRange`, `__nd_camera_request` /
  `_release` / `_label` / `_enumerate`, `__nd_mic_release`. Those traced act
  on the caller's own document. `__ndMse*` and `__ndNativeRange` were not
  audited. M1 is the same class of problem: internal state the page can
  write.
- **Camera labels.** `ns_cam_label` (`src/js.c:12824`) and `ns_cam_enumerate`
  (`:12849`) return device ids and labels with no permission check and are
  reachable as `__nd_camera_label()` / `__nd_camera_enumerate()`.
- **`ns_computed_lookup`** keeps `computed` from `src/js.c:20345` across
  recursive lookups, some of which flush layout. No use after a flush was
  established.
- **`js_cage_huge_alloc`** truncates the chunk count to `uint32_t`
  (`quickjs-cage.c:520-526`). No caller can pass a size that large today.
- **Shared-buffer refcount lives inside the cage**, directly before the data
  (`src/js_cage.c:18-21`, `68-77`), against the stated rule of no
  bookkeeping in the cage.
- **`cascade_rollback_value` and `var_rollback_match`** (`src/css.c:33834`,
  `31484`) recurse once per chained `revert-rule` / `revert-layer` with no
  depth bound. They are tail calls, so only an unoptimised build can
  overflow the stack.
- **WebGL null-pixel uploads** fall back to an uninitialised texture when
  the zero buffer cannot be allocated (`src/webgl.c:3561-3568`,
  `4721-4722`).
- **PBKDF2** has no bound on iterations or output length and runs
  synchronously (`src/webcrypto.c:1269-1292`).
- **List-marker text** is truncated into 64 bytes with `g_strlcpy`
  (`src/paint.c:9178`), which can split a UTF-8 sequence before shaping.
- **Still present from the earlier §4:** enabled but unused vertex
  attributes are unchecked (`src/webgl.c:992-994`); the `getParameter`
  default branch forwards unknown names into a 32-int array
  (`src/webgl.c:1723-1729`).

## 5. Status of earlier findings

| Item | Description | Status at `983d178b` |
|------|-------------|----------------------|
| J5 | MutationObserver `subtree` crosses into framed documents | **fixed**: `ns_mut_target_covers` (`src/js.c:33165-33175`) stops at an embedded document |
| N2 | WebGL2 ImageData over-read | fix re-read and holds (`src/webgl.c:3581-3594`, `3684-3697`) |
| N3 / J7 | Sub-frame rewrites the tab URL; history state is tab-wide | open (`src/js.c:18401`, `18499-18541`; `src/libnordstjernen.c:793-803`); not re-run |
| N4 | WebGPU canvas context holds a raw canvas node pointer | open (`src/webgpu.c:4961-4989`, `2421-2424`, `1367`) |
| N5 | Compression dictionaries not partitioned by top-level site | open (`src/compression_dictionary.c:264`, `334`) |
| N7 | Audio-helper verbs other than `open` / `reload` unchecked | open (`src/gtk/procview.c:984-990`) |
| N8 | Unchecked `snprintf` chaining in `render_request_json` | open (`src/rproc_http.c:764-792`) |
| J6 | Script-animation events go to the current realm | open (`src/js.c:38315-38343`) |
| J8 | `document.pointerLockElement` is tab-wide | open (`src/js.c:45925`) |
| J9 | Selection text is tab-wide | open (`src/js.c:18099-18106`) |
| J10 | Internal natives callable by page script | open as a class; see §4 |
| J12 | `document.referrer` on a synthetic document | open (`src/js.c:68345-68356`) |
| J13 | Camera labels without permission | open (`src/js.c:12824`, `12849`) |
| J14 | Fetch response headers filtered against the wrong document | open (`src/js.c:10765-10766`) |
| J15 | Style pointer across a layout flush | open, upgraded; see §3 |
| J16 | `pending_scrollend` raw node pointers | open (`src/js.c:37722-37757`) |
| J17 | `127.` prefix counts as trustworthy | open and wider; see C2 |
| F3 | `import()` of `data:` / `blob:` skips CSP `script-src` | open (`src/js.c:72813-72849`, check at `72883-72889`) |
| F4 | CSP `'self'` matches `blob:` URLs | open (`src/csp.c:271-274`) |
| F5 | Top-level navigation to `data:` | open (`src/libnordstjernen.c:997-1009`) |
| F6 | Transfer detaches a `WebAssembly.Memory` buffer | open (`src/js.c:17868-17886`, `28211-28212`); no memory impact |
| F7 | Undefined float-to-int conversions | open where spot-checked (`src/libnordstjernen.c:221-224`, `src/svg.c:2147-2148`) |
| S1 / G6 | Shell trusts the renderer-reported URL | open (`src/gtk/procview.c:866-879`, `3929`) |
| S3 / G7 | Config rewrite starts remote control | open (`src/gtk/remotectl.c:1009-1027`) |
| S11 | Private mode falls back to a guessable `/tmp` directory | open (`src/net.c:487-499`) |

J11, S2, S4, S5 and S6 were not re-checked.

## 6. Checked and found sound

- **Regexp capture widening (`18d8fc40`).** Opcode operands, the header, the
  length walkers and the executor all use the 16-bit width. The count is
  capped at 65535 at parse and prescan. Capture storage is heap-allocated.
  No `uint8_t` or 255-sized array remains on a capture path. The bytecode
  cache is memory-only, so no old-format bytecode is read back.
- **Cage allocator.** Free and usable-size reject interior, misaligned,
  unallocated and double-freed pointers. Guard regions sit on both sides.
- **WebGL.** `wgl_transfer_bytes` matches or over-estimates what GL reads
  for every format and type. `readPixels`, `getBufferSubData`, uniform
  arrays, multi-draw lists and compressed uploads bound their sources and
  destinations. Argument conversions that can run script re-assert the
  context, and draw validation runs after them.
- **WebGPU.** `getMappedRange` hands script a copy; write-back is skipped on
  a length mismatch or a destroyed device.
- **MSE.** Every unit length from the WebM and ISO-BMFF splitters is at most
  the pending length; chunk offsets are re-checked at each use.
- **Tables.** Spans are clamped to 100 at parse, columns capped, every column
  index checked, the collapse-grid allocation overflow-checked.
- **Blur.** Sigma and radius are clamped to 512 before window arithmetic.
- **WebCrypto.** Key, IV, tag and counter lengths are validated; output
  buffers cover cipher expansion.
- **WebSocket.** Framing is libcurl's; assembly is capped.
- **TLS and redirects.** Peer and host verification default on. The only
  downgrade is the opt-in certificate override, gated on a top-level
  navigation to a host that is not HSTS-pinned, and it drops the cookie
  header (`src/net.c:6362-6374`). Cross-origin redirects strip
  `authorization`, `cookie` and `proxy-authorization`.
- **Cookies.** `__Host-` / `__Secure-` prefixes, `Secure`, `HttpOnly`,
  `SameSite`, public-suffix domain rejection and partitioning by top-level
  site are enforced (`src/cookies.c:859-1097`).
- **`postMessage` data.** Structured clone produces receiver-realm objects in
  both directions, including `Map`, `Set`, `Error`, typed-array buffers and
  transferred ports. Only the exception path leaks (C1).
- **Privileged schemes.** Privileged `about:` pages, `view-source:` and
  `file:` are refused to web content and are not followed on redirect. The
  about-page builders escape history titles, bookmark names and URLs.
- **Extensions.** File reads are confined by `ns_ext_safe_path`; storage
  directories are per extension and skipped in private mode.
- **Control servers.** WebDriver and remote control bind loopback only;
  remote control needs a 32-byte random token. The renderer channel has no
  listening endpoint.
- **SQL.** History and cookies use bound parameters throughout.

## 7. Suggested order of work

1. **Port C1 and M1** to the other editions (`Security-Fix:` slugs
   `cross-realm-exception-leak` and `webaudio-param-event-count-overread`).
2. Move AudioParam automation state out of page-writable properties.
3. **M3**, at least the refusal of WebGL2 on non-robust contexts.
4. **J15**, **N4** and **J16**: the three open raw-pointer lifetimes.
5. **N3 / J7** and the rest of the tab-wide state class (J6, J8, J9, J14).
6. Make `__`-prefixed bindings engine-private by default (J10, J13).
7. Read the parts no review has reached: the CSS cascade and selector
   matching, `var()` / `calc()`, flex, grid and inline layout, `paint.c`,
   the seccomp filter and download filename handling.
