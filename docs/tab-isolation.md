# Tab isolation and renderer boundaries

This note records the current isolation shape in Nordstjernen, how the
codebase got there, and the next useful steps. Process-per-tab is
implemented (see *Current state*); what remains is:

- browser-process services for networking, cookies, cache, and storage,
  so a renderer can be made credential-less;
- a finer process-per-site boundary;
- a seccomp profile, or a pre-sandbox zygote, for the shell.

## Current state

Nordstjernen now runs each tab's engine in **its own sandboxed renderer
process** (`nordstjernen-renderer`); the GTK shell is a thin
display/input client. The process-per-tab boundary described later in
this document is the implemented default. The sections from *Per-tab
worker status* through *Recommended order* were written for the original
single-process GTK engine and are kept as history; *First implementation
step* onward describes the process boundary as built. An
opt-in `--single-process` flag limits the browser to one tab with one
ordinary renderer process; see
[`single-process-mode.md`](single-process-mode.md).

Engine state is per renderer process:

- parsed DOM (`ns_browser`'s `doc`), layout tree, computed-style table, scroll,
  focus, history cursor, CSP, image/video cache, and animation state;
- one QuickJS runtime and context, created by `ns_js_new()` in
  `src/js.c`;
- classic Dedicated Workers created by that page get their own QuickJS
  runtime, GLib main context, and OS thread, with message handoff back
  to the owning page;
- the page is driven over IPC by `libnordstjernen` (`src/libnordstjernen.c`)
  and painted into the shared-memory framebuffer.

The old process-global active JavaScript pointer has been replaced by a
thread-local `GPrivate` slot. JavaScript still runs on the main thread,
but the runtime ownership model is now compatible with future tab-owned
thread work as long as each QuickJS runtime is only entered from one
thread at a time.

Networking is already asynchronous. `ns_net_fetch_async()` runs blocking
curl work on a GTask worker pool, with global and per-origin throttles,
then returns results to the main loop. Shared browser state is guarded
where it is currently shared:

| Subsystem | Global state | Lock / owner |
| --- | --- | --- |
| HTTP cache | `cache.c` SQLite handle | `g_cache_mutex` |
| History | `history.c` SQLite handle | `g_history_mutex` |
| Cookies / HSTS / curl share | `net.c` globals | `g_hsts_lock`, curl share locks |
| Bytecode cache | `bytecode_cache.c` `g_mem` | `g_lock` |
| Font cache | `font.c` entries | main thread / Pango |
| Config | `config.c` `g_cfg` | browser process |

Cookies are partitioned by registrable site, and same-origin, CORS, CSP,
mixed-content, and SRI checks exist. Within one renderer process these
are policy boundaries,
not memory-safety boundaries.

## Per-tab worker status (historical)

This section describes the single-process GTK engine, which ran one
serial worker thread per tab. The renderer process has no such thread:
page body decode, HTML parse, and stylesheet parse run on the renderer's
main thread, and still-image decode runs on the GLib `GTask` pool while
`async_image_decode` is on (the default); see
[threading.md](threading.md).

The per-tab worker was deliberately serial. That keeps publication order
simple, avoids intra-tab worker races, and still lets different tabs
prepare pages and resources in parallel.

Worker-owned jobs at the time:

| Work item | State | Main-thread publication |
| --- | --- | --- |
| Main page body decode | done | generation-checked completion |
| Main page HTML parse | done | parsed tree is accepted only for the live tab and fetch generation |
| Top-level external CSS parse | done | stylesheet publication, cascade invalidation, and layout invalidation remain on the main loop |
| Still-image decode | mostly done | worker returns owned pixels; main loop creates `ns_texture` / `GdkTexture` |
| Video poster decode | mostly done | same pixel-buffer handoff for still images |
| Animated GIF decode | mostly done | worker returns owned frame pixels; main loop creates animation textures |
| GDK-Pixbuf image fallback | mostly done | worker decodes to pixels, with temporary texture download only as a compatibility fallback |
| `@import` expansion | main thread | expansion inspects live tab policy: CSP, mixed content, stylesheet grouping |
| JavaScript parse / execution | main thread | QuickJS is per tab, but DOM bindings mutate live tab state |
| Dedicated Worker JavaScript | worker thread | classic worker scripts run in a separate QuickJS runtime; message payloads cross through QuickJS object serialization without bytecode or SharedArrayBuffer flags |
| Layout | main thread | depends heavily on Pango measurement |
| Paint | main thread | Cairo / Pango draw directly into the tab drawing area |

Fetch generation checks are now used across page and stylesheet
publication, including Stop/cancel paths. Old network or worker results
should not overwrite a newer navigation once `fetch_gen` has advanced.

## What threads isolated (before process-per-tab)

Before the renderer split, the threaded architecture gave useful fault
containment inside the program structure:

- tab DOMs, style state, JS heaps, scroll/focus state, and resource
  caches are separate objects;
- CPU-heavy page preparation, CSS parsing, and some image decoding can
  run away from the GTK main loop;
- shared caches and network state are behind explicit locks;
- process-wide seccomp and Landlock reduce the whole process's system
  call and file-system reach after startup;
- existing `fork()` / `socketpair()` patterns in the watchdog and media
  broker show that process machinery is available in the tree.

That was not a renderer sandbox. Any memory-safety bug in the browser
process could still read or corrupt another tab's DOM, JS heap, cookies,
or cache state. Threads improve responsiveness and reduce accidental
cross-tab coupling, but they do not provide a security boundary — which
is why the engine moved into per-tab renderer processes.

## Main blockers

Display-list snapshotting is the next architectural boundary. Today the
paint path walks live layout and DOM-related structures on the main
thread. Before layout or script can safely move further off-thread, the
renderer needs an immutable paint snapshot that the main thread can
consume without touching mutable tab state.

Pango is the layout blocker. Text measurement and shaping are spread
through `src/layout.c`, `src/paint.c`, `src/font.c`, selection, export,
and canvas text. The safest near-term strategy is to keep Pango-owned
objects on the main thread and introduce a batched text-measurement
service. Per-thread FT2-backed `PangoContext`s may be viable later, but
they are a cross-platform risk and should be proven behind a small
experiment before layout depends on them.

JavaScript is per-tab but not off-thread-ready. QuickJS runtimes are
separate, but DOM bindings currently mutate the live tree that layout,
paint, events, and selection also inspect. Script movement should wait
until DOM mutation, layout invalidation, and paint snapshots have a
clear ownership protocol.

Dedicated Workers are isolated from the DOM but not from the browser
address space. They are useful for page-authored background JavaScript
and for proving per-runtime thread ownership, but they do not protect
tabs from native memory-corruption bugs. Keep worker APIs conservative:
same-origin classic scripts first, no SharedArrayBuffer, no bytecode
deserialization, no transfer lists until ownership rules are explicit,
and no worker `fetch` until network callbacks are guaranteed to resolve
on the worker context through teardown.

Process isolation remains the security boundary. A future renderer
process should not read cookies, history, cache databases, downloads, or
general disk state directly. Those capabilities need browser-process
services and a small IPC protocol before renderer subprocesses can be a
meaningful sandbox.

WebGL used to complicate the split because it bound GL state to GTK's
process-local GL context. It now draws through the toolkit-independent
offscreen context in `src/glctx.c` inside the renderer, which prepares GL
before sealing its sandbox (`ns_gl_prepare_for_sandbox`).

## Suggested next steps

1. Finish the worker resource handoff.

   Still images and animated GIF frame sequences now return owned pixels
   from the worker. The remaining cleanup is to remove the rare
   temporary texture-download compatibility fallback and keep
   display-backend objects entirely on the main thread.

2. Tame stylesheet imports.

   Keep CSP, mixed-content, SRI, and stylesheet-group decisions on the
   main thread, but move any pure `@import` parse work that does not
   require live tab policy behind the worker boundary. Preserve the same
   tab id and fetch generation checks used by top-level CSS.

3. Add a display-list snapshot.

   Introduce a compact immutable display-list type with commands for
   backgrounds, borders, text runs, images, replaced elements, clips, and
   scroll offsets. First generate and paint it on the main thread. The
   first win is not threading; it is making paint consume a stable
   snapshot instead of live DOM/layout state.

4. Put Pango behind a measurement interface.

   Start with a main-thread text measurement service and an explicit
   cache key: text, font description, width, language, direction,
   spacing, white-space behavior, and relevant text-transform state.
   Batch worker requests so layout can ask for many measurements with
   one main-loop round trip. After that exists, run a small
   per-thread-Pango experiment separately.

5. Move inactive-tab layout behind snapshots.

   Once display lists and text measurement are explicit, try worker-side
   layout for inactive tabs first. Active-tab layout should move only
   after input latency, selection, scrolling, and incremental relayout
   still feel correct.

6. Define renderer IPC before adding renderer processes (done — see
   *First implementation step* below).

   Specify the message surface first: navigation commit data,
   subresource requests, response bodies, input, resize, timers, console
   messages, storage requests, display-list snapshots, and pixel buffers.
   Keep cookies, HSTS, HTTP cache, history, downloads, and persistent
   storage in the browser process.

7. Add process-per-site renderers after the IPC boundary is real
   (process-per-tab has landed; per-site has not).

   Use a forked renderer or zygote model that fits the current no-`execve`
   seccomp shape, then narrow Landlock/seccomp inside each renderer.
   Process-per-site is the right default compromise for memory and
   process count. Crashes should become tab/site failures, not whole
   browser failures.

## Recommended order

The practical sequence is:

1. texture fallback cleanup for rare decoder paths;
2. stylesheet import cleanup;
3. main-thread display-list generation and painting;
4. Pango measurement service;
5. inactive-tab worker layout;
6. renderer IPC protocol (done);
7. process-per-site sandboxed renderers (process-per-tab done).

This keeps each step small enough to verify locally while moving the
code toward the only true security boundary: a credential-less renderer
process with browser-owned networking, cookies, cache, and storage.

## First implementation step (landed)

A minimal out-of-process renderer pipeline now exists as the seed of the
IPC boundary (steps 6–7), independent of the threading cleanups above:

- `src/rproc_http.{h,c}` + `src/ipc_http.{h,c}` — the parent/client side: an
  **HTTP/1.1 + JSON control channel plus a shared-memory framebuffer**. Each
  operation is a `POST` (`/open`, `/render`, `/tick`, `/link`, `/click`,
  `/key`, `/hover`, `/scroll`, `/find`, `/viewport`, `/select`, `/eval`,
  `/console`, `/media`, `/print`, `/export`, `/quit`, and others) with a
  small JSON body and JSON reply; only `/render` touches the shared
  framebuffer (geometry in `X-*` headers; a tiled render lists its tiles
  in the body, see [Rendering.md](Rendering.md)). The
  framebuffer is passed by descriptor, not by name: Linux backs it with an
  anonymous `memfd_create`, other Unix with an immediately-`shm_unlink`ed
  `shm_open` segment passed over the control socket with `SCM_RIGHTS`; Windows
  creates an unnamed inheritable file mapping and passes the handle. Renders
  are coalesced (one in flight), so the shell copies each frame out of the
  single shared buffer into its display surface off the UI thread — no slot or
  reference-count bookkeeping. This replaced an earlier opaque binary-struct
  protocol: readable on the wire, no slot-lifetime footgun, and identical
  performance because the data plane is shm.
- `src/renderer_http.c` — the renderer process (`nordstjernen-renderer`),
  `fork`+`execv`'d by the POSIX parent and `CreateProcess`'d by the Windows
  parent (control over `stdin`/`stdout`). It holds one open `libnordstjernen`
  page and services the HTTP operations above
  (`ns_browser_open_viewport` / `ns_browser_render_argb32` /
  `ns_browser_link_at` / …). It links no GUI toolkit.
- `src/gtk/procview.{h,c}` + `src/gtk/procwindow.{h,c}` — the **GTK** frontend's
  process-per-tab UI, the **only** GTK renderer (the former in-process engine
  renderer has been removed). `NsProcView` is a `GtkDrawingArea` that shows the
  renderer's tiles as textures (`src/gtk/pagelayers.c`, or full frames with
  `NS_TILES=0`), runs `rproc_http` IPC on a per-view worker thread
  (`GAsyncQueue` + `g_idle_add` replies, results guarded by a refcount and a
  closed flag), and handles wheel/keyboard scroll, link clicks, hover, text
  selection, find-in-page, a context menu, the DevTools console, save/export,
  and open-in-new-process-tab; `NsProcWindow` is a `GtkNotebook` of them with a
  toolbar, address entry, app menu, settings, and bookmarks. The proc-mode GTK
  shell applies Landlock only, no seccomp, so it can spawn renderers and
  create POSIX shm — the full sandbox lives in the renderer processes (see
  *Proc-mode security posture* below).

This started as a proof of concept: Linux/macOS use `fork`+`execv`, Windows
uses `CreateProcess` and inherited pipes. The framebuffer is no longer a
global named object — it is an anonymous `memfd`/unlinked shm fd passed over
`SCM_RIGHTS` (Unix) or an unnamed inherited mapping handle (Windows) — so no
predictable name appears in `/proc/PID/cmdline`, `/dev/shm`, or the `Local\`
namespace. The eventual POSIX sandboxed renderer should move to the
fork/zygote + inherited-mapping shape that fits the no-`execve` seccomp
policy (step 7); Windows should grow the matching restricted token / Job
Object / AppContainer shape separately. The control channel
now has bounded child teardown, client-side renderer restart in the proc
view, and tiled rendering, which repaints only the tiles whose pixels
changed (see [Rendering.md](Rendering.md)). IPC
reply reads are bounded on Linux/macOS and Windows so a wedged renderer
becomes a failed request that the client can close or restart, not an
unbounded browser-side wait. The proc view still uses a synchronous
request/reply rproc protocol, but it dispatches those calls off the UI
thread so page load, scroll, resize, and hover paths no longer block on an
IPC round trip.

The renderer now sandboxes itself. After it maps the framebuffer and
initialises the engine, but before it reads its first request,
`nordstjernen-renderer` calls `ns_browser_sandbox` (a glib-free entry point on
`libnordstjernen`) to apply Linux Landlock + seccomp confinement (a Seatbelt
profile on macOS, process mitigation policies on Windows). Untrusted HTML/CSS/JS is therefore parsed, scripted, laid out, and
painted under a loaded syscall filter, with the renderer's filesystem reach
narrowed to what the engine needs. The `NS_NO_SANDBOX` / `NS_NO_SECCOMP`
overrides are honoured, and where Landlock is unavailable the call degrades
gracefully. Next: browser-process broker
services for networking, cookies, cache, and storage so the renderer can be
made credential-less rather than fetching and persisting on its own.

## Proc-mode security posture (GTK shell)

With process-per-tab the **default**, the GUI shell and the
renderers have different threat models, so they are confined differently.

- **Renderer processes** process untrusted bytes and carry the real sandbox:
  each `nordstjernen-renderer` applies Landlock + seccomp (`ns_browser_sandbox`)
  after mapping its framebuffer and before opening any page. This is unchanged
  and is where confinement matters most.
- **The shell** (`nordstjernen` in proc mode) blits
  framebuffers and forwards input; it parses no untrusted content. It must,
  however, `fork`/`execv` renderer processes, which the engine's full sandbox
  blocks (the seccomp allow-list has no `execve`). So the GTK shell applies
  **Landlock only**, widened just enough for its job: the renderer's directory
  is made executable (`ns_security_add_exec_dir`) and `/dev/shm` is left
  writable (`ns_security_add_writable_dir`) to cover the non-`memfd`
  shared-memory fallback, while seccomp is skipped. The renderer framebuffer
  is normally an anonymous `memfd` handed over the control socket, so it needs
  no `/dev/shm` entry.
- **seccomp for the shell** is deferred: a shell profile would need to permit
  `execve`/`clone` (to spawn renderers), which removes seccomp's most valuable
  guarantee, and the shell's attack surface is low (no untrusted parsing). A
  tailored profile, or a pre-sandbox zygote that forks renderers so the shell
  can keep the no-`execve` filter, is the eventual fix.
- **Watchdog ("guarddog")** supervision is active in proc mode (the default):
  when `watchdog_enabled` is set, `ns_watchdog_run_supervisor` (`src/watchdog.c`,
  called from `src/gtk/appmain.c`)
  spawns the thin engine-free shell and restarts it on crash or hang. Engine
  crashes are additionally contained structurally: the engine runs in renderer
  processes, a renderer crash is confined to its tab, and `NsProcView`
  transparently restarts a dead renderer (bounded so a reliably-crashing page
  can't thrash). A shell crash is rare (thin, no engine) and the supervisor
  recovers it.

Net effect vs. a single-process browser: the renderer confinement is
**stronger** (per-process, credential-narrowing is the next step), while the
shell process is **less** confined (no seccomp, wider Landlock) because it must
spawn renderers and map POSIX shm. That trade is sound while the shell stays a
thin display/input client; it tightens further with a renderer broker and a
shell-specific seccomp profile or zygote.

## Live JavaScript in the renderer

The renderer links the full engine, so an opened page already runs blocking,
deferred, and async scripts and fires `DOMContentLoaded`/`load`, with a short
settle loop that pumps `requestAnimationFrame` and due timers. What was missing
was keeping JavaScript alive *after* that settle: nothing iterated the GLib
context, so `setTimeout`/`setInterval`, promise resolutions, and animations
scheduled for later never advanced, and a deferred DOM update never reached a
later frame.

`ns_browser_tick(browser, budget_ms)` on `libnordstjernen` fixes that. Before
each full `RENDER`, `nordstjernen-renderer` calls it to fire due timers,
deliver pending fetch/XHR and promise jobs, run `rAF` and CSS animations, and
relayout if the DOM changed, all bounded by a fixed 16 ms budget (deferred
while a scroll is in progress; `/tick` runs it without rendering). A latent
bug surfaced while wiring this
up: `ns_drain_mutations` notifies the embedder's mutate callback and then
clears the dirty flag, but `libnordstjernen`'s callback was a no-op, so
timer/event-driven mutations were swallowed before `ns_js_consume_mutated`
could see them. The callback now marks the browser dirty so the tick relayouts.

This makes JS-driven content correct on the next render. For continuous
animation the renderer reports `X-Anim` on each render reply while
`ns_browser_animating()` is true (active CSS transitions/animations or pending
`requestAnimationFrame`), and the shell drives a frame loop for as long as it
is set; otherwise it renders on demand (see [Rendering.md](Rendering.md)).

## Shared memory for images (evaluated)

Considered giving decoded images their own shared-memory channel. Not worth it:
the renderer already composites decoded image pixels into the shared-memory
**framebuffer**, which is the only surface the client needs — images therefore
already cross the process boundary as part of the zero-copy frame, with no
extra IPC. Sharing *decoded image buffers* between renderer processes would
also cut against the per-process isolation that is the whole point of
process-per-tab (a shared image cache becomes shared, attacker-reachable state
across sites) for a small, niche memory saving. The framebuffer stays the one
shared region; image decoding stays private to each renderer.
