# Rendering and scrolling

Nordstjernen renders each tab out of process. The GTK app is a thin
shell (`src/gtk/procview.c`) that spawns one sandboxed
`nordstjernen-renderer` process per tab (`src/renderer_http.c`) and drives it
over a tiny **HTTP/JSON control channel plus a shared-memory framebuffer**
(`src/rproc_http.c`, `src/ipc_http.c`). The engine — HTML parse, the CSS
cascade, layout into a live `ns_box` tree, and Cairo/Pango paint — runs entirely
inside the renderer child; the UI process only blits the framebuffer and
forwards input. The shape was chosen over an opaque binary struct protocol
because it is readable on the wire, has no slot-lifetime bookkeeping to get
wrong, and costs nothing in throughput: the pixels never travel over it.

Every control message is a plain HTTP `POST` with a small JSON body — `/open`,
`/render`, `/tick`, `/link`, `/click`, `/key`, `/hover`, `/scroll`, `/find`,
`/viewport`, `/select`, `/eval`, `/console`, `/media`, `/print`, `/export`,
`/quit`, and others, all dispatched by `ns_renderer_session_handle` in
`src/renderer_serve.c` — so the protocol is readable in a trace and trivial to
extend. Only the rendered pixels travel through shared
memory. The framebuffer is not a global named object: the shell backs it with an
anonymous `memfd_create` (Linux), an immediately-unlinked `shm_open` segment
(other Unix), or an unnamed inheritable file mapping (Windows), and hands the
renderer the descriptor/handle over the control channel rather than a name.

## The render path

For each frame the shell `POST`s `/render` with the target size, scroll offset,
and a `scale` that maps CSS pixels to device pixels. The renderer:

1. pumps pending JS/animation work for a 16 ms budget (`ns_browser_tick`),
   deferring it while a scroll is in progress;
2. paints the requested viewport region of the layout tree straight into the
   shared-memory framebuffer in Cairo-native ARGB32
   (`ns_browser_render_argb32` over `src/paint.c`), with no intermediate
   surface;
3. replies with `X-W`/`X-H`/`X-Stride`/`X-Anim` (plus page-size and scroll)
   headers and an empty body — the pixels are already in the shell's address
   space via the shared mapping.

Renders are coalesced so at most one is in flight; the shell's per-view worker
thread copies the frame out of the shared mapping into its display surface
(a texture on GTK) before issuing the next render. The copy runs off the UI
thread; the data plane never crosses the socket. While `ns_browser_animating()`
is true
(active CSS transitions/animations or pending `requestAnimationFrame`), the
shell drives a frame loop; otherwise it renders on demand (scroll, resize,
hover restyle, find, click result).

## Tiles and scrolling

The GTK shell asks for the page as tiles instead of one viewport frame
(`"tiles":1` in the `/render` body). The renderer (`src/renderer_tiles.c`)
splits the page into layers by paint order and paints them separately; the
shell picks the tile height (`NS_PV_TILE_H`, 256 device rows):

- **document layers** are the content that scrolls with the page. They are
  cut into horizontal tiles of that height at full viewport width;
- **fixed layers** hold one `position: fixed` subtree each, in viewport
  coordinates;
- **sticky layers** hold one `position: sticky` subtree each (scrollport is
  the viewport), painted at its static position, plus the clamp that moves it
  (`ns_box_sticky_y_model`).

`ns_paint_plan_layers` finds the fixed and sticky subtrees, checks that each
one is painted at the root or inside a stacking context that only orders
paint (no opacity, transform, clip or filter), and records them in paint
order. Content painted after the n-th of them goes to document layer n, so
drawing the layers in order gives the same picture as one full paint. A page
that cannot be split this way, for example one with `background-attachment:
fixed`, a modal dialog in the top layer or sticky inside sticky, is painted
as full frames.

The reply body lists the layers and tiles; the pixels are in the shared
mapping. Tiles carry a generation number that changes whenever the page's
appearance may have changed (a tick, hover, caret blink, a different size or
scale). For a new generation the renderer repaints only the visible tiles;
it hashes each tile and answers `keep` when the shell already holds the same
pixels, so an animation in one corner uploads only the tiles it touches.
Tiles outside the view are refreshed when they scroll into view.

The shell (`src/gtk/pagelayers.c`) keeps the tiles as textures and moves them
itself when the user scrolls, so scrolling does not wait for the renderer or
for the page's scripts, and a scroll step uploads at most a few new tiles. A
wheel gesture that starts over an inner scroll container is still sent to the
renderer, which scrolls that container. A playing video in a document layer
is sent as a layer of its own and drawn under the tiles; the page paints a
transparent hole where it shows (`paint_video_layer_hole` in `src/paint.c`).
`NS_TILES=0` turns tiles off and brings back full frames.

Resizing re-lays-out the page for the new CSS-pixel viewport width in the
renderer (`/viewport` → `ns_browser_set_viewport`), re-evaluating
`@media` queries, viewport units, and fluid widths.

## Isolation

Because the engine runs in a separate, sandboxed process, a crash or hang in
untrusted page content cannot take down the UI; the shell detects a dead
renderer and restarts the tab. The renderer confines itself with
`ns_browser_sandbox` — Landlock + seccomp on Linux, a Seatbelt profile on
macOS, process mitigation policies on Windows; see
[security-model.md](security-model.md).

The framebuffer is the only memory a renderer shares. Decoded images are
composited into it and stay private to the renderer that decoded them;
there is no image cache shared between renderers, which would be state
one site's renderer could write and another's would read.

## Long documents

Layout and document install must stay linear in document size; a 15 MB
single-page document (html.spec.whatwg.org, ~470k nodes) is the working
reference. Two hot spots to keep in mind when touching these paths:

- `ns_doc_class_index_build` / `ns_doc_tag_index_build` append in
  document order without per-insert dedup scans (`g_doc_index_building`
  in `src/dom.c`). Only incremental mutation goes through the ordered
  insert with its linear duplicate check — never route a full rebuild
  through it.
- Static positions for `position: absolute` boxes are resolved in one
  postorder sweep per containing block
  (`static_abs_y_precompute` in `src/layout.c`), not per element.
  Per-element walks (`static_abs_y_walk`) remain only as the fallback
  for targets whose document position cannot be located by node rank;
  they prune subtrees that follow the target.

With `NS_PROFILE=1` (set in the renderer's environment), the engine prints
`[profile]` lines to stderr (`restyle`, `relayout`, `paint`, `tiles`, and
`css.idx=`/`css.cascade=`, among others); `css.cascade=` is expected to
dominate large static pages once layout is linear.
