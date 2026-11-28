# Benchmarking Nordstjernen on Speedometer 3.1

The official [Speedometer 3.1](https://browserbench.org/Speedometer3.1/)
harness loads every workload inside an `<iframe>` and drives it through
`frame.contentDocument` / `frame.contentWindow` after `frame.onload`.
Nordstjernen now supports this: setting an iframe's `src`/`srcdoc`
fetches and parses the document, splices it under the iframe node so it
styles, lays out, and runs its own scripts, marks the iframe
`data-nd-frame-loaded` so the UA stylesheet renders it, and dispatches
`load`. `HTMLIFrameElement` exposes `contentDocument` (documentElement /
body / head / getElementById / querySelector / querySelectorAll /
defaultView) and a scoped `contentWindow` facade with frame-local
`location` and `history`. Individual official suite runs produce scores.

Multi-suite aggregate runs now drive to a geomean score as well — the
runner reloads its one test iframe per suite/iteration and the engine
keeps up. (Earlier these appeared to hang; that was the per-reload
document-index and orphan/pinned-wrapper growth turning each reload
O(n^2), since fixed, plus the headless settle running ~2x the
`--settle-ms` budget, so a too-short timeout killed the run before it
finished.) Example aggregate over the eleven light TodoMVC frameworks
(ES5, ES6-Webpack, WebComponents, React, React-Redux, Backbone, Vue,
jQuery, Preact, Svelte, Lit), `iterationCount=1`:

```sh
./builddir/src/gtk/nordstjernen \
  'http://localhost:8124/index.html?suite=TodoMVC-JavaScript-ES5,TodoMVC-JavaScript-ES6-Webpack,TodoMVC-WebComponents,TodoMVC-React,TodoMVC-React-Redux,TodoMVC-Backbone,TodoMVC-Vue,TodoMVC-jQuery,TodoMVC-Preact,TodoMVC-Svelte,TodoMVC-Lit&startAutomatically=true&iterationCount=1'
```

`window` is still the one shared global, so cross-suite state (framework
globals, window listeners) can still leak between suites in a single
aggregate run; treat the aggregate as indicative, and prefer per-suite or
fresh-process runs when comparing the engine over time.

```sh
# Serve the harness and run it headless to a score:
git clone --depth 1 --branch release/3.1 \
    https://github.com/WebKit/Speedometer.git /tmp/spdm
( cd /tmp/spdm && python3 -m http.server 8124 & )
./builddir/src/gtk/nordstjernen --headless --dump=text --settle-ms=60000 \
    'http://localhost:8124/index.html?suite=TodoMVC-JavaScript-ES5&startAutomatically=true&iterationCount=1'
```

For per-suite engine tracking we still also drive each Speedometer 3.1
*workload* directly via `scripts/speedometer-bench.sh`, which fetches
Speedometer's `release/3.1` sources at runtime, generates a page driver
from them, replays Speedometer's own per-suite interaction steps against
the top-level document, and times each suite. Nothing third-party is
vendored into this repository.

## Running

```sh
scripts/dev.sh build
scripts/speedometer-bench.sh            # every loadable TodoMVC suite
scripts/speedometer-bench.sh complex    # only paths containing "complex"
```

Each suite is run `NS_ITERS` times (default 3) and the median total is
reported. Per phase the driver records `sync` time (the interaction loop) plus
`async` time (a forced layout flush on the next frame), matching the real
runner's `measurementMethod = "raf"` measurement. `domNodes` is the live
element count at the end of the run.

Other environment overrides: `NS_SETTLE` (headless settle budget in ms,
default 8000), `NS_BIN` (browser binary), `NS_PORT` (local HTTP port,
default 8123), and `NS_SPEEDOMETER_DIR` (scratch checkout of the
Speedometer sources). `scripts/speedometer4-bench.sh` is the equivalent
driver for the Speedometer `main` (4.0-alpha) workloads.

## What runs

Suites that drive end to end and produce a score in the **official iframe
harness** (run individually, `?suite=<name>&startAutomatically=true`):

- All seven light-DOM TodoMVC frameworks and their `-Complex-DOM` variants:
  vanilla JS (ES5, ES6-Webpack), React, Angular, Vue, Preact, Svelte.
- **React-Redux** and **WebComponents** — after the window-event-handler and
  custom-element fixes (`src/js.c`); WebComponents builds its list with `new
  TodoItem()` into shadow roots, which now yields real element instances.
- The non-TodoMVC **Charts-chartjs**, **Charts-observable-plot**,
  **Editor-CodeMirror**, **Editor-TipTap**, and **NewsSite-Nuxt** suites.
- **Lit** - after custom-element construction stopped reusing the upgrading
  element for ordinary `EventTarget` subclasses created inside custom element
  constructors.
- **jQuery** and **Backbone** — after giving iframe scripts their own scoped
  `document` (`src/js.c`), running an iframe's classic scripts in one shared
  scope (so Backbone's cross-file `var app` is shared), and serializing
  `<script>` innerHTML unescaped (`src/dom.c`) so Handlebars/Underscore
  templates compile to real markup.

Each iframe's classic `<script>`s run concatenated inside one
`(function(document){...})` wrapper, with `document` bound to a facade scoped
to the iframe's subtree (a per-call current-document swap); module scripts
keep their own scope. This also dropped Next.js's head-manager errors to
zero. `window` is still the shared global, so the remaining gaps are:

Still imperfect:

- **NewsSite-Next** — past the head errors, but Next.js hydration doesn't
  drive to completion. **React-Stockcharts-SVG** currently renders its
  Stockcharts fit wrapper placeholder but never creates the SVG crosshair
  cursor, and **Perf-Dashboard** still reaches the harness UI but returns no
  score. Multi-suite aggregate runs are also polluted by stale window
  `load`/`hashchange` listeners and framework globals left by previous suites.

Current fresh-process official-suite snapshot on Windows (2026-06-03):
17 of 20 suites score, geomean **2.4340**. This is not the official
single-page aggregate score; it is the comparable per-suite score while the
shared global limitation remains.

## Snapshot (release build, Windows 11, 2026-11-28)

Official Speedometer 3.1 harness, each of the 20 default suites in a fresh
headless process with `iterationCount=1`: all 20 score, geomean **3.0**.
The slowest are NewsSite-Next (0.74), NewsSite-Nuxt (0.81), Perf-Dashboard
(1.27), Editor-TipTap (1.57) and TodoMVC-jQuery (1.87).

JetStream (WebKit `main`), one test per fresh process: 64 of 83 tests
score. The whole-suite page does not get past preloading (it stops at
resource 508 of 677). The scores are those of an interpreter: richards 39,
splay 44, Babylon 87, raytrace 17, typescript-octane 3.8.

Page loads, headless, private mode: google.com paints in 0.3 to 0.4 s;
youtube.com in 2.1 to 2.9 s, of which 1.5 to 2.2 s is script.

A gdb sampling profile of the news-site and editor suites pointed at four
costs, fixed in this pass:

- **`elementFromPoint` shaped every inline box.** The form-control hit
  walk built a text layout for each inline box with any attribute, then
  asked it for the point. It now builds one only for a box that holds a
  form control.
- **`for-in` and `Object.keys` walked the realm list.** Hiding the engine's
  own globals from enumeration looked the object up among all realms on
  every call; a non-global object is now rejected by its class first.
- **A full collection every 250 ms.** Retiring a frame's realm and sweeping
  orphaned nodes each force a collection. The wait before the next forced
  one is now 20 times what the last one took, 250 ms at least.
- **The Windows heap.** A quarter or more of the samples were inside
  `RtlAllocateHeap` and `RtlFreeHeap`, reached through `g_malloc`. The
  renderer now carries a manifest that selects the segment heap.

Serial runs, median of three, score (higher is faster):

| Suite                       | before | fixes | + segment heap |
|-----------------------------|-------:|------:|---------------:|
| NewsSite-Next               |  0.752 | 0.815 |          0.845 |
| NewsSite-Nuxt               |  0.782 | 0.866 |          0.912 |
| Editor-TipTap               |  1.751 | 1.871 |          1.889 |
| TodoMVC-Angular-Complex-DOM |  4.890 | 5.247 |          5.513 |
| TodoMVC-jQuery              |  1.875 | 1.889 |          1.948 |
| Perf-Dashboard              |  1.501 | 1.373 |          1.531 |
| geomean                     |  1.556 | 1.619 |          1.698 |

JetStream and the two page loads did not move outside run-to-run noise.

A second pass the same day looked at runs of several iterations, which is
what the official harness does, and at where a single iteration spends its
time:

- **The first iteration is the slow one.** Editor-TipTap scores 1.9 with
  `iterationCount=1` and 12.6 with six: its first iteration lays out about
  800 KB of French prose twice, 160 to 170 ms each, and every later layout
  of the same text takes 4 to 7 ms from the shaping and measurement caches.
  A page of 180,000 unique words lays out cold in 410 ms, about 2.3 µs a
  word, so this is shaping throughput and not a stall.
- **A style not seen before costs about a millisecond.** On a cold font
  map each new font size or weight adds 1.1 to 1.3 ms to the first layout
  and each new family about 3.2 ms.
- **Retiring a frame's realm walked every wrapper.** With 40 iterations a
  profile put 12% of the time in `ns_js_retire_realms`, most of it asking
  each detached node wrapper which realm its prototype's constructor
  belongs to. The walk now remembers the answer per prototype, so the
  property lookup runs once per prototype instead of once per wrapper.
  Six-iteration runs of four suites: geomean 4.125 to 4.200.
- **Start-up.** A headless run of a one-paragraph page takes about 380 ms
  of wall time, of which the page itself is 36 ms and `--version` alone is
  125 ms. The rest is process and library start-up and was not broken down.

What remains, largest first:

- **Layout is rebuilt whole on every flush.** NewsSite-Next forces 163
  layouts of about 8 ms each on a 1,000-node page where 13 nodes were
  restyled; that is most of the suite's time. Reusing the box tree for
  subtrees whose style and content did not change is the largest lever.
- **Allocation churn in layout.** Each of those layouts allocates and frees
  the whole box tree, its attribute lists and font descriptions.
- **Script speed.** JetStream and youtube.com are bound by the interpreter.
- **JetStream coverage.** 19 tests fail or time out, most of them the
  large WebAssembly and start-up workloads, and the whole-suite preload
  stalls.

## Snapshot (release build, Windows 11, GTK 4.22, 2026-06-05)

Median total ms, lower is faster. The "prior" column is the 2026-06-03
Linux snapshot after the first cascade pass; "total" is after the
2026-06-05 render-pipeline work (preorder rank map for absolute
positioning, fused pseudo-element cascade gathering, intrinsic-width
caching, offscreen paint culling).

| Suite                         |  prior | total |   add | complete | delete | DOM   |
|-------------------------------|-------:|------:|------:|---------:|-------:|------:|
| TodoMVC-Angular               |    230 |   522 |   493 |        9 |      5 |   641 |
| TodoMVC-JavaScript-ES5        |    228 |   660 |   575 |       77 |     47 |    43 |
| TodoMVC-React                 |    352 |   673 |   504 |       13 |    103 |    24 |
| TodoMVC-Svelte                |    361 |   669 |   522 |      109 |     60 |    20 |
| TodoMVC-Preact                |   2610 |  2808 |  1049 |     1300 |    562 |    60 |
| TodoMVC-jQuery                |      — |  1665 |   636 |      534 |    303 |    41 |
| TodoMVC-Backbone              |      — |   604 |   530 |       30 |     24 |    50 |
| TodoMVC-React-Complex-DOM     |   8681 |  1851 |  1704 |       14 |     97 |  6632 |
| TodoMVC-Angular-Complex-DOM   |   8052 |  1597 |  1583 |        9 |      5 |  7249 |
| TodoMVC-JavaScript-ES5-Complex|   8607 |  1675 |  1498 |       90 |     60 |  6651 |
| TodoMVC-Svelte-Complex-DOM    |   8663 |  1572 |  1422 |      103 |     47 |  6628 |
| TodoMVC-Preact-Complex-DOM    |  10353 |  3750 |  1988 |     1115 |    647 |  6678 |
| TodoMVC-jQuery-Complex-DOM    |      — | 10229 |  9174 |      746 |    371 |  6649 |
| TodoMVC-Backbone-Complex-DOM  |      — |  5802 |  5779 |       36 |     34 |  6658 |

The Complex-DOM suites dropped ~5x with the 2026-06-05 pass (the small-DOM
columns are not comparable across the two machines). The remaining outliers
are jQuery-Complex and Backbone-Complex, whose add phases do per-item
selector queries over the whole document.

The numbers are not comparable to an official Speedometer score — this driver
has no warmup suite and no geomean aggregation. They are a self-consistent
per-workload measurement for tracking the engine over time.

## Snapshot (release build, Linux, GTK 4.14, 2026-06-05) — custom-property cascade

`scripts/speedometer-bench.sh`, median total ms over 3 iterations, lower is
faster. "prior" is the Linux baseline before this pass; "total" is after
sharing inherited custom-property tables by reference instead of deep-copying
them per element (`build_vars_for_element` in `src/css.c`).

| Suite                          | prior | total |
|--------------------------------|------:|------:|
| TodoMVC-Backbone-Complex-DOM   |  5914 |  1392 |
| TodoMVC-jQuery-Complex-DOM     |  9646 |  2833 |
| TodoMVC-Preact-Complex-DOM     |  3581 |  2649 |
| TodoMVC-Vue-Complex-DOM        |  2041 |  1081 |
| TodoMVC-Lit-Complex-DOM        |  2263 |  1121 |
| TodoMVC-Angular-Complex-DOM    |  1232 |   433 |
| TodoMVC-React-Complex-DOM      |  1459 |   518 |
| TodoMVC-Svelte-Complex-DOM     |  1468 |   550 |
| TodoMVC-JavaScript-ES5-Complex |  1503 |   548 |
| TodoMVC-ES6-Webpack-Complex    |  1560 |   546 |

The Speedometer "complex" CSS declares ~1600 custom properties on `:root` that
inherit to every element. `build_vars_for_element` used to materialise that
full table for each element that declared even one local `--var`
(`g_strdup` of every key and value), so a single full-document re-cascade did
tens of millions of string copies. A per-element profile attributed ~80% of
the whole cascade to this one step. The inherited table is immutable once
built, so it is now shared by reference: each element holds a small
copy-on-write layer (`ns_var_map`) of just its own overrides chained to the
parent's map, and `var()` lookups walk the chain. The `@property`-registered
path (where non-inheriting/initial semantics force a flat table) is unchanged.
This cut the var-heavy complex suites 2–4x with no change to the light suites,
and the same speedup carries over to the Speedometer `main` (post-3.1) beta
workloads, which share the complex CSS.

## Profiling

The `*-Complex-DOM` variants cost ~30-40x their small-DOM twins, and the cost
is almost entirely the `Adding100Items` phase. The ES5-Complex suite uses no
framework, so its time is pure engine. The shape is a **full-document restyle
on every DOM mutation**: each of the 100 inserts (each forced layout flush)
re-cascades the whole tree, so the cost scales as O(items x nodes x rules).
The small-DOM suites stay fast because their trees are tiny; the complex-DOM
suites carry ~7000 static nodes that are re-styled 100 times over.

A 150-sample gdb profile of the add phase originally attributed ~41% to glibc
allocator churn (`_int_malloc` / `free` / `malloc_consolidate`) and ~30% to
GLib hashing behind selector matching, under `cascade_walk` →
`gather_matches_impl` (`src/css.c`). These changes cut the allocator churn:

- **Skip empty pseudo-element passes.** `cascade_walk` ran eight pseudo-element
  gather passes (`::before`/`::after`/`::first-letter`/…) per element, each
  allocating four arrays, even when no stylesheet styled that pseudo-element.
  Each stylesheet now records a bitmask of the pseudo-elements its selectors
  target (built with the rule index), and passes for absent pseudo-elements
  are skipped entirely.
- **Reuse cascade scratch arrays.** The four per-element match arrays were
  `g_array_new`/`g_array_free`d for every node on every relayout; they are now
  cleared-and-reused scratch buffers.
- **Skip the per-element variable map** when no custom properties are in scope
  (`build_vars_for_element` returns NULL instead of allocating an empty hash
  table for every node).
- **Build the box-lookup index lazily.** `getBoundingClientRect` no longer
  builds a whole-document box index to satisfy a single query per reflow.

Together these drop the complex-DOM suites ~20-29% (see the table).

A fresh 60-sample profile after these changes shows the remaining cost is the
**inherent full-document re-cascade**: leaf time is dominated by the glibc
allocator (`_int_malloc`/`free`/`malloc_consolidate`, ~45%) and `g_str_hash` /
`g_hash_table_lookup` from selector-index matching, with `ns_style_free` high
in the inclusive frames — i.e. allocating and freeing ~7000 computed styles
(and their `ns_css_value`s) on each of the 100 reflows. The two levers that
remain, both larger and correctness-sensitive, are:

1. **Incremental restyle** — re-cascade only the mutated subtree (plus the
   siblings/ancestors that structural selectors depend on) instead of the whole
   document, turning the add phase from O(items x nodes) toward O(items).
2. **Refcount `ns_css_value`** — `cascade_for` deep-copies every matched and
   inherited value into each element's style and `ns_style_free` frees them
   all; sharing immutable values by refcount would remove most of the per-node
   alloc/free churn. This needs an audit that no site mutates a value in place.

To reproduce the profile:

```sh
meson setup builddir-prof --buildtype=debugoptimized -Dlto=false
meson compile -C builddir-prof
# in one shell, start a long-settle run of a complex-DOM suite, then:
scripts/sample-profile.sh <pid> 150
```

## querySelector key-index fast path (2026-06-05)

A sampling profile of the `TodoMVC-*-Complex-DOM` add phases (now that the
cascade allocator churn is tamed by value refcounting and shared inherited
custom-property tables) showed the dominant remaining engine cost shifting
from the cascade to `ns_walk_first_match` — the `querySelector` /
`querySelectorAll` slow path, which walked the whole ~6600-node document and
tested every element. The framework suites issue per-item selector queries
over the top-level document on each of the 100 inserts.

`ns_query_selector_simple` already short-circuits bare `#id`, `.class`, and
`tag` queries through the document id/class/tag indices. The slow path now
applies the same idea to compound and descendant selectors via a **rightmost
key bucket**: for a single-selector query whose subject (rightmost) compound
carries a positive id/class/tag, candidates are gathered from that document
index and verified with the full `ns_css_selector_matches` (which already does
right-to-left combinator matching), instead of walking the subtree. The
candidate set is provably identical to the walk's — the document indices skip
`<template>` content with the same guard the walk uses — so only ordering
shares the existing index-order caveat the simple fast path already accepts.

The bucket path is gated to top-level (`root == document`) queries: for an
element-scoped query on a small subtree whose rightmost key is common
document-wide, walking the small subtree is cheaper than scanning a whole
document-wide bucket, so element-rooted queries keep the walk. The cheap
`#id` single-node lookup still applies for any root.

Median-of-3 over the loadable Speedometer 3.1 TodoMVC workloads
(`scripts/speedometer-bench.sh`, release build, Linux), lower is faster:

| Aggregate                    | before | after | delta |
|------------------------------|-------:|------:|------:|
| Sum of all suite totals      |  26135 | 25232 | -3.5% |
| Sum of Complex-DOM totals    |  17932 | 17279 | -3.6% |

The query-bound suites move most: jQuery-Complex-DOM -7.5%, Preact-Complex-DOM
-7.6%, Preact -6.5%, React -6.2%, Backbone -3.8%. The same engine path serves
the Speedometer `main` (4.0-alpha, "speedometer next") workloads, which share
the TodoMVC suites; `scripts/speedometer4-bench.sh` drives those directly
(it fetches the `main` branch and generates a page driver from
`default-tests.mjs` the same way the 3.1 harness does from `tests.mjs`).

## Batched `innerHTML` replacement records (2026-06-07)

The jQuery, Backbone, Preact, Vue, and Lit TodoMVC paths repeatedly replace a
list container's full `innerHTML` during add/complete/delete phases. The DOM
setter previously cleared children and appended parsed children one at a time,
calling the child-change recorder for every top-level removed and added node.
Each call invalidated the selector-query cache and walked id/class/tag
subtrees for document indices, so a full-list replacement paid repeated hash
and tree-walk overhead before the page immediately forced layout.

`ns_element_set_innerHTML` now batches that full replacement: removed children
are collected, parsed children are appended, then the query cache and document
indices are updated once. MutationObserver delivery still receives childList
records with `addedNodes` / `removedNodes` arrays, matching the operation's
aggregate shape.

One-iteration Windows snapshot on the 22 TodoMVC suites that produced a
duration, using the repo drivers (`NS_ITERS=1`, `NS_SETTLE=10000`):

| Driver | Before score | After score | Geomean before | Geomean after | Delta |
|--------|-------------:|------------:|---------------:|--------------:|------:|
| Speedometer 3.1 (`release/3.1`) | 0.8283 | 0.9896 | 1207.2 ms | 1010.5 ms | +19.5% |
| Speedometer main / 4.0-alpha | 0.9591 | 0.9992 | 1042.7 ms | 1000.8 ms | +4.2% |

The largest 3.1 movers were Angular-Complex, Backbone, jQuery, Preact, Svelte,
and ES6-Webpack. In 4.0-alpha the same change improved the aggregate score,
with jQuery, Backbone, Vue, ES5, and ES6-Webpack moving most; jQuery-Complex
remains noisy and should be rechecked with multi-iteration medians before
treating one run as a regression.

## Cache inline-SVG textures before the document defs walk (2026-06-26)

(Historical: the `build_box` / `ns_collect_svg_defs` path described here
was later replaced when SVG moved to the in-engine renderer,
`src/svg.c`; the measurements still record what the fix bought at the
time.)

A fresh sampling profile of the `*-Complex-DOM` add phase (the
`scripts/sample-profile.sh` poor-man's sampler against a long-settle stress
run that repeatedly forces a full-document relayout) put **~40% of leaf time
in `ns_collect_svg_defs`** and the `__strcmp_evex` it drives — not in the
cascade at all. The Speedometer "complex" big-DOM carries **769 inline
`<svg>` elements** and **zero** `<defs>`-type elements
(`symbol`/`linearGradient`/`radialGradient`/`clipPath`/`mask`/`filter`/`pattern`).

`build_box`'s `<svg>` path decodes each inline SVG to a texture cached by node
pointer (`nd-inline-svg:%p`), but it called `ns_svg_outer_with_defs(n)` —
which walks the **whole document from the root** to gather referenced `<defs>`
and serializes the element's outer HTML — *before* peeking that cache. So every
relayout rebuilt a throwaway XML string for all 769 SVGs, each walk testing
~6650 nodes against seven tag names (~36M `strcmp`s per relayout) only to
discard the result on the cache hit. The add phase forces ~100 relayouts, so
this was paid ~100×.

The decoded texture is already frozen at first sight (keyed by node pointer,
never re-decoded), so building the XML on a hit never affected output. The fix
peeks the texture cache **first** using the node-pointer key and only runs the
document walk + serialize + decode on a miss (`build_box`, `src/layout.c`).
Layouts 2..N over a given SVG become pure cache hits; only the first layout
pays the walk.

`scripts/speedometer-bench.sh`, `NS_ITERS=1`, `NS_SETTLE=12000`, release build,
Linux, median total ms (lower is faster):

| Suite                                | before |  after |  delta |
|--------------------------------------|-------:|-------:|-------:|
| TodoMVC-Backbone-Complex-DOM         |   2506 |   1686 | -32.7% |
| TodoMVC-jQuery-Complex-DOM           |   3633 |   2767 | -23.8% |
| TodoMVC-Svelte-Complex-DOM           |   1109 |    900 | -18.8% |
| TodoMVC-JavaScript-ES5-Complex-DOM   |   1127 |    924 | -18.0% |
| TodoMVC-JavaScript-ES6-Webpack-Complex| 1086 |    901 | -17.1% |
| TodoMVC-React-Complex-DOM            |   1207 |   1041 | -13.8% |
| TodoMVC-Vue-Complex-DOM              |   1559 |   1407 |  -9.8% |
| TodoMVC-jQuery (light)               |   1274 |   1164 |  -8.7% |
| TodoMVC-Lit-Complex-DOM              |   2093 |   1924 |  -8.1% |
| TodoMVC-Angular-Complex-DOM          |    852 |    800 |  -6.1% |
| **Sum of all 22 loadable suites**    | **27719** | **24795** | **-10.5%** |

The light-DOM suites stay flat (their trees carry no big-DOM SVG), confirming
the change only removes work. After the fix the sampler no longer shows
`ns_collect_svg_defs`; the remaining add-phase cost is the genuine
full-document re-cascade (`match_simple` / `gather_matches_multi` plus
allocator churn), i.e. the incremental-restyle lever already noted above. The
same engine path serves the Speedometer 4.0-alpha TodoMVC workloads, which
reuse these complex pages.

## Incremental restyle (2026-06-26)

The full-document re-cascade noted above was the dominant remaining cost: every
forced layout re-matched all ~6,650 nodes of the complex DOM even when a single
todo was added. `cascade_walk` now reuses the previous pass's computed style for
any element whose subtree is provably unaffected by the mutations since the last
cascade, recomputing only the dirty subtrees.

How it works:

- The attribute and childList mutation recorders (`ns_js_record_attr_change`,
  `ns_js_record_child_change` in `src/js.c`) call `ns_css_mark_restyle_dirty(parent)`,
  marking the changed element's parent as a dirty root (covers the element, its
  siblings — for `+`/`~`/`:nth-child` — and their subtrees).
- `cascade_walk` threads an `under_dirty` flag. A node with no dirty ancestor
  **clones** its style from the previous table (`ns_style_clone_shared`, sharing
  the already-refcounted `ns_css_value`s) instead of gathering and matching
  rules. Anything inside a dirty subtree recomputes fully. A recomputed node
  marks its descendants dirty, so inherited values stay correct.
- The previous styles are kept in an independent table owned by `ns_css_compute`
  (cloned from the prior `out`), so the borrowed-table lifetime of the
  free-before-recompute callers is not an issue.

Safety: the optimisation is **on by default** but falls back to a full cascade
whenever it cannot prove equivalence — a stylesheet uses `:has()` (ancestor
matching depends on descendants), container queries are in play, the
focus/hover/active element moved, or the stylesheet set changed. Pages that use
`:has()` or `@container` skip the machinery entirely (no clone-maintenance
overhead). Set `NS_NO_INCR_RESTYLE=1` to force the full cascade.

Verification: the `--dump=layout` output is **byte-identical** (after
normalising non-deterministic inline-SVG cache-key pointers) between the full
cascade (`NS_NO_INCR_RESTYLE=1`) and incremental modes after an
add-40 / complete-every-3rd / delete-5 interaction, across vanilla ES5/ES6,
React, and Vue complex and light suites.

`scripts/speedometer-bench.sh complex`, `NS_ITERS=3`, release build, Linux,
median total ms (lower is faster), full cascade vs incremental:

| Suite                                 |  full |  incr |  delta |
|---------------------------------------|------:|------:|-------:|
| TodoMVC-WebComponents-Complex-DOM     |  2279 |  1586 | -30.4% |
| TodoMVC-Backbone-Complex-DOM          |  1699 |  1214 | -28.5% |
| TodoMVC-Svelte-Complex-DOM            |   905 |   662 | -26.9% |
| TodoMVC-JavaScript-ES6-Webpack-Complex|   897 |   665 | -25.8% |
| TodoMVC-JavaScript-ES5-Complex-DOM    |   922 |   699 | -24.2% |
| TodoMVC-React-Complex-DOM             |  1032 |   788 | -23.6% |
| TodoMVC-Angular-Complex-DOM           |   764 |   588 | -23.0% |
| TodoMVC-Lit-Complex-DOM               |  2031 |  1642 | -19.1% |
| TodoMVC-Vue-Complex-DOM               |  1380 |  1188 | -13.9% |
| TodoMVC-jQuery-Complex-DOM            |  2968 |  2611 | -12.0% |
| **Sum of complex suites**             | **14875** | **11643** | **-21.7%** |

A 5-iteration ES5-Complex run moved 910.7 -> 666.5 ms (-26.8%); the steady-state
reuse rate is ~97% of nodes cloned per pass. This is a coarse version of what
production engines do (Blink invalidation sets, Gecko/Stylo's parallel restyle +
rule tree): the conservative "dirty the parent subtree" rule recomputes more
than strictly necessary, and the per-pass clone-maintenance could be replaced by
storing the computed style on the node. Those are the next steps.

### Reuse instead of clone (2026-06-26)

The first incremental-restyle cut cloned every clean node's computed style each
pass (once in `cascade_walk`, once rebuilding the previous-styles table) — the
clone was the dominant remaining cost. `ns_style` now carries a refcount, and at
render zoom ~= 1 (where the post-cascade passes do not mutate computed styles —
`ns_anim_observe` is read-only and `render_apply_zoom` is a no-op) a clean node
**reuses** the previous pass's exact style struct (`ref++`) instead of cloning
it, and the previous-styles table is rebuilt by ref-counting rather than
copying. `ns_css_set_render_zoom` (called from `src/render.c`) gates this; when
zoom != 1 the cascade falls back to the clone path so the zoomed font-size value
is never shared back into the cache.

Median of 3, complex suites, full cascade vs clone-incremental vs reuse:

| Aggregate                  |  full | clone | reuse | reuse vs full |
|----------------------------|------:|------:|------:|--------------:|
| Sum of 10 complex suites   | 14875 | 11643 | 10708 |        -28.0% |

ES5-Complex over 5 iterations: 910.7 (full) -> 666.5 (clone) -> 610.6 (reuse).
Output remains byte-identical to the full cascade (verified across vanilla
ES5/ES6, React, and Vue complex and light suites). The reuse path shares style
structs across passes, which is safe only because the cascade output is
immutable at zoom ~= 1; the zoom gate preserves that invariant.
