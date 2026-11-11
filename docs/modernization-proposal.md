# Modernization proposal

Follow-up to `docs/modernization-report.md`. It turns the decisions taken on
that report into an ordered plan. Everything here is from reading the code at
`1.0.47-dev`; nothing was built or run for it, so each item starts by
confirming its premise in the running browser.

## Decisions this plan implements

1. Properties that only parse get a real implementation, not a `@supports`
   opt-out.
2. Add `@starting-style`, the View Transitions API, the Custom Highlight API,
   speculation rules and `fetchLater()`.
3. Remove: `-ms-flexbox` / `-ms-grid` display values, the `ms*` / `moz*`
   fullscreen aliases, `chrome.loadTimes()` / `chrome.csi()`, the `<blink>`
   animation, the iframe script path that wraps code in `with(window){}` and
   sniffs `webpackChunk`, and the ShadyDOM hooks.
4. Bring the stale docs in line with the code.

## What the code says about each item

Three things found while preparing this plan change its shape.

- **Scrollbars are overlay scrollbars.** They are painted over the content
  (`src/paint.c:11379-11383`, 8 px, 5 px for `thin`) and layout reserves no
  space for them. With overlay scrollbars the specification gives
  `scrollbar-gutter` nothing to do, so the property is already correct and
  needs no code. The same holds for `forced-color-adjust`: `forced-colors`
  always evaluates to `none` (`src/css_media.c:917`). Both leave the
  "parse-only" list as "correct as is"; the compatibility docs should say so.
- **Scrolling is done in the window process.** The renderer hands the shell a
  list of scroller rectangles (`src/libnordstjernen.c:3123-3194`) and the
  shell moves them. `scroll-behavior`, `overscroll-behavior` and
  `touch-action` are therefore one piece of work: carry three more values per
  scroller across that channel and honour them in `src/gtk/procview.c`.
- **The `with(window){}` path is not a rare fallback.** A frame gets a real
  context only when it is cross-origin or its parent already has one
  (`src/js.c:55390-55391`). The top window is never "native", so every
  same-origin frame directly under the top document takes the legacy path
  (`src/js.c:72123-72130`). Removing it means making the native realm the
  only realm, which is the largest item in this plan.

## Phase 1: removals and docs (small, independent, do first)

Each is one commit. Build, smoke-launch, load a few mainstream sites.

| Item | Where | Note |
|------|-------|------|
| `-ms-flexbox`, `-ms-inline-flexbox`, `-ms-grid` | `src/css.c:10601-10606` | Keep the `-webkit-` entries; real sites still ship them |
| `mozRequestFullScreen`, `msRequestFullscreen`, `mozCancelFullScreen`, `msExitFullscreen`, `mozFullScreenElement`, `msFullscreenElement`, `mozFullScreen` | `src/js.c:56135-56136`, `:66233-66247` | Keep the `webkit*` aliases; check the surrounding lines for `moz`/`ms` event-name aliases and drop those too |
| `chrome.loadTimes()`, `chrome.csi()` | `src/js.c:62133-62134` and the two functions | Decide in the same commit whether an empty `window.chrome` object is still wanted |
| `<blink>` animation | `src/css.c:32257-32258` | The element stays an inline `HTMLUnknownElement` |
| ShadyDOM hooks | `src/js.c:63219-63220`, exemptions at `:61005`, `:61388` | Verify on a Polymer 1/2 page that it now takes its native shadow DOM branch |
| Stale docs | report section 5.6 | `AGENTS.md` (gdk-pixbuf fallback, external player), `docs/tab-isolation.md:85`, `scripts/pack-windows.sh:134`, `scripts/pack-linux.sh:215`, `meson.build:436-438`, `docs/Linux.md:54`, the `mpv` recommendation in `nordstjernen.spec:54` and its two doc mentions |
| One spelling for the hardware-decode switch | `src/media_os_mf.c:315` | Accept `NS_MEDIA_NO_HW_DECODER`, the documented name |
| Compatibility docs | `docs/CSS-compatibility.md`, `docs/HTML-compatibility.md` | Refresh from 1.0.23 / 1.0.16; record `scrollbar-gutter` and `forced-color-adjust` as correct no-ops |

Rerun `scripts/wpt-score.sh` for `fullscreen` and `css/css-display` after the
first two rows and commit the regenerated score files with them.

## Phase 2: make the parse-only properties real

Ordered by value to real pages. Size S is under a day, M a few days, L a week
or more.

| # | Property | Design | Files | Size |
|---|----------|--------|-------|------|
| 2.1 | `scroll-behavior: smooth`, and `behavior: 'smooth'` in `scrollTo` / `scrollBy` / `scrollIntoView` | Programmatic and fragment scrolls on a smooth scroller ease to the target on the animation tick instead of jumping; a user scroll or a new programmatic scroll cancels the ease. Disabled under `prefers-reduced-motion` | `src/js.c` (scroll entry points near `:53530`, `:45139`), `src/engine.c`, `src/anim.c` | M |
| 2.2 | `overscroll-behavior` | Add the x/y values to each scroller rectangle sent to the shell; a scroller with `contain` or `none` swallows the leftover delta instead of passing it to its parent | `src/libnordstjernen.c:3123-3194`, `src/gtk/procview.c`, the IPC struct | S-M |
| 2.3 | `touch-action` | Same channel as 2.2: the shell skips its pan / pinch gesture over a region whose `touch-action` forbids it, and pointer events flow to the page instead | same as 2.2, plus Android | M |
| 2.4 | `field-sizing: content` | Text inputs, textareas and selects take their intrinsic size from the current value (or placeholder) and re-layout on edit; `min-`/`max-` sizes still clamp | `src/layout.c` (form-control intrinsic sizing), edit path invalidation | M |
| 2.5 | `resize` | A grip in the corner of an `overflow` box whose `resize` is not `none`; dragging sets an inline-size / block-size override that layout respects, limited by `min-`/`max-`. Textareas default to `both` in the UA sheet | `src/paint.c`, `src/layout.c`, hit-testing in `src/libnordstjernen.c`, `kUa` | M |
| 2.6 | `background-blend-mode` | Paint each background layer with the cairo operator for its mode inside an isolated group; reuse the keyword-to-operator mapping of `mix-blend-mode` (`src/paint.c:9116`) | `src/paint.c` | S |
| 2.7 | `text-decoration-skip-ink: auto` | Underlines and overlines break where they cross glyph ink: take the run's glyph path, widen it, and subtract it from the decoration strip. `auto` is the initial value, so this changes every underline; `none` keeps today's output | `src/paint.c` near `:4160-4195` | M |
| 2.8 | `interpolate-size: allow-keywords` | A transition or animation whose endpoint is `auto`, `min-content`, `max-content` or `fit-content` interpolates against the used size from layout. Needs a "used size at this keyword" query from layout into `src/anim.c` | `src/anim.c:699-765`, `src/layout.c` | L |
| 2.9 | `view-transition-name`, `view-transition-class` | Delivered by View Transitions (3.2) | | |

Also from the report, same class of problem and worth scheduling here:
`text-wrap: pretty` (grammar only, `src/css.c:668`) and
`content-visibility: auto` (containment only, no skipping).

Verification for each: a small page under `data/render-tests/` that takes the
`@supports` branch and shows the behaviour, opened in the browser, plus the
matching WPT directory through `scripts/wpt-score.sh`.

## Phase 3: new features

### 3.1 `@starting-style` (M)

`transition-behavior: allow-discrete` already works (`src/anim.c:749`), so
this is the missing half of entry animations for popovers, dialogs and
`display: none` toggles.

- Parse `@starting-style` as a grouping rule, also nested inside a style
  rule, and expose `CSSStartingStyleRule`.
- When an element has no before-change style (first style after being
  inserted, or after `display: none`), compute a second style with the
  starting rules included and give it to `observe_transition`
  (`src/anim.c:765`) as the "from" style.
- Verify with the guidance's entry/exit example for a popover and a dialog.

### 3.2 View Transitions, same-document (L)

The pseudo-element names already parse (`src/css.c:2241-2270`).

1. `document.startViewTransition(callback)` returning a `ViewTransition`
   with `ready`, `finished`, `updateCallbackDone`, `skipTransition()`.
2. Capture: before the callback, snapshot the root and each element with a
   `view-transition-name` into a surface, with its box geometry.
3. Run the callback, lay out, then build the pseudo tree
   (`::view-transition` > `-group(name)` > `-image-pair` > `-old` / `-new`)
   as generated boxes over the page, old images from the snapshots and new
   ones painted live.
4. UA animations: cross-fade plus a group animation of size and transform
   from old to new geometry, all through the existing keyframe machinery so
   author CSS can override them.
5. Later, separately: `view-transition-class`, `:active-view-transition`,
   cross-document transitions (`@view-transition { navigation: auto }`).

Step 1 with an immediate, un-animated update is a useful first commit: pages
that call the API then work instead of throwing.

### 3.3 Custom Highlight API (M)

- `Highlight` (a set of ranges with `priority` and `type`), `CSS.highlights`
  (a map-like registry), and the `::highlight(name)` selector.
- Paint through the code that already draws `::selection`
  (`src/css.c:37767`, `s->selection`): resolve registered ranges to text
  runs, apply colour, background and text-decoration only, ordered by
  priority.
- The same range-to-run painter then serves `::target-text`,
  `::spelling-error` and `::grammar-error`, which today are recognised names
  only (`src/css.c:2239`).

### 3.4 Speculation rules (M, in two steps)

- **Prefetch.** Parse `<script type="speculationrules">` (list rules and
  document rules with `where` / `eagerness`) and the `Speculation-Rules`
  response header, then feed matching same-site URLs into the existing
  prefetch path (`src/engine.c:455`) with `Sec-Purpose: prefetch`. Respect
  the cookie and tracking-protection settings and skip everything when the
  user has data saving or reduced network use set.
- **Prerender.** Not proposed now. It needs a hidden renderer per candidate
  page and activation semantics; revisit after prefetch has proved useful.
  `HTMLScriptElement.supports('speculationrules')` should report true only
  once prefetch works.

This sends requests the user did not ask for. It should have a Settings
switch and stay within the existing link-prefetch behaviour, not extend it
to cross-site URLs without one.

### 3.5 `fetchLater()` (S-M)

`navigator.sendBeacon` already queues a request that outlives the page
(`src/js.c:60039`). `fetchLater(url, { activateAfter })` returns a
`FetchLaterResult` with `activated`, holds the request in the renderer, and
sends it through the same path when the timer fires or the document is
discarded, whichever is first. Add the per-origin body quota from the
specification so it cannot be used to queue unbounded data.

## Phase 4: one iframe realm

Goal: delete the wrapper, the `var`-name scanner, the `webpackChunk` test and
the prototype snapshot / restore helpers (`src/js.c:71411-71760`,
`:72123-72130`), about 400 lines, by running every frame in its own context.

1. **Measure.** Log which path each frame takes over a session of normal
   browsing and the `html/browsers` and `html/semantics/embedded-content`
   WPT areas. This confirms the reading above and gives a baseline score.
2. **Switch behind a debug environment variable.** Make
   `ns_iframe_make_realm_context` choose the native realm for same-origin
   frames under the top document as well. The hard part is what the legacy
   path gave for free by sharing one context: same-origin parent and child
   see each other's objects directly, `instanceof` across frames, shared
   custom-element and event-target state. The cross-realm cloner
   (`ns_realm_cloner`, `src/js.c:54465-55320`) has to cover those cases for
   a top-level parent.
3. **Fix regressions** found against the baseline until the native path
   scores at least as well.
4. **Make it the default, then delete** the legacy path and the variable in
   a separate commit, so a bisect can tell the two apart.

Size L, risk high: frames are on most pages (embeds, consent dialogs, ads,
payment forms). This is the only item that should not be started until
phases 1-3 are quiet.

## Suggested order

1. Phase 1, all rows.
2. `@starting-style` (3.1), `background-blend-mode` (2.6),
   `overscroll-behavior` (2.2), `scroll-behavior` (2.1).
3. `field-sizing` (2.4), `fetchLater` (3.5), Custom Highlight API (3.3).
4. `resize` (2.5), `text-decoration-skip-ink` (2.7), `touch-action` (2.3),
   speculation-rules prefetch (3.4).
5. View Transitions (3.2), which brings 2.9.
6. `interpolate-size` (2.8).
7. Phase 4.

Each step ends with `meson compile -C builddir`, a `-Dquickjs=quickjs-ng`
build when `src/js.c` changed, a smoke launch, the WPT area rerun, and a
changelog line.

## Open questions

- Speculation rules: is a Settings switch wanted, and should it default on?
- `window.chrome`: keep an empty object for feature sniffing, or remove it
  with `loadTimes` / `csi`?
- `mpv` in `nordstjernen.spec`: drop the recommendation, since the shell no
  longer launches an external player?
- `text-decoration-skip-ink` changes how every underline looks. Acceptable
  as the default, as the specification says, or behind `auto` only once it
  has been compared on real pages?
