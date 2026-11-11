# Modernization report

Review of Nordstjernen at commit `e1f948e7ec34009bcc380685a85c4fa09eb325e7`
(1.0.47-dev, 2026-11-11) against
[GoogleChrome/modern-web-guidance](https://github.com/GoogleChrome/modern-web-guidance)
at commit `4dd968349e54ca45a05dd5c818fb035c9054b022` ("Release v1.0.0",
2026-11-10). Read-only review: no source file was changed.

## 1. Summary

### Scope and method

- The guidance repository was cloned and read as reference data. Nothing in
  it was executed. Its `legacy-scan` CLI (which the guidance itself says
  covers about 20% of the catalogue) lives in the npm package, not in the
  repository, and was **not run**; every legacy finding below comes from
  reading and grepping this tree by hand.
- First-party tree: 227 files, about 284,600 lines under `src/*.c`,
  `src/*.h`, `src/gtk/`, `src/audio/` (excluding the forked `lexbor`,
  `quickjs`, `quickjs-bellard`, `web-text-lib`, `wamr` trees).
- Findings carry `path:line` for lines that were opened, and the grep used
  where the finding is a count.

### What was read in full, sampled, or not covered

| Area | Coverage |
|------|----------|
| Guidance: `README.md` feature catalogue, `SKILL.md`, `css/css.md`, `html/html.md` | read in full |
| Guidance: `css-layout`, `responsive-design`, `color`, `typography`, `motion`, `visual-effects`, `forms`, `performance`, `security`, `privacy`, `dark-mode`, `web-components`, `accessibility` | rule lines and headings extracted, not every paragraph |
| Guidance: the other ~190 use-case guides | one-line descriptions plus Baseline statements aggregated by grep; individual guides opened only where cited |
| `src/about_style.h`, internal page templates in `src/net.c` (error page, start page, settings, directory listing, view-source, easter eggs), `src/history.c` style and script, `src/safebrowsing.c` interstitial, `src/html.c` viewers | read |
| UA stylesheet `kUa` / `kUaQuirks` (`src/css.c:32060-32277`) | sampled (lists, form controls, dialog, popover, legacy elements) |
| `src/css.c` (39k lines), `src/js.c` (74k), `src/layout.c`, `src/paint.c`, `src/anim.c`, `src/net.c` | structure plus targeted greps; **not** read line by line |
| `data/js/polyfills.js` (15.5k lines) | header and idiom counts only |
| `meson.build`, `meson.options` | read (`meson.build` partially) |
| `docs/` | `CSS-compatibility.md` header; other docs only by grep for stale references |
| `scripts/`, `.github/workflows/` | skimmed (shell options, interpreter lines, action versions) |
| `data/render-tests/`, `data/fixtures/`, `docs/demos/` | listed only; they exercise legacy features on purpose |
| `android/`, `java/`, `npm/`, `pip/`, `debian/`, `packaging/`, `data/i18n/` | not reviewed beyond the greps noted |
| Forked/vendored trees | not audited |

Nothing was built or run, so every "supported" entry in section 4 means "the
code path exists", not "verified to render correctly".

### Top recommendations, ranked by value over effort

1. **Stop advertising CSS the engine only parses.** About 12 properties the
   guidance pushes authors toward are accepted and serialized with no
   behaviour behind them, so `@supports` answers yes and the author's
   fallback is skipped (4.2). Small change per property, high compatibility
   value.
2. **Make `color-scheme` real.** `light-dark()` and the system colors ignore
   the element's `color-scheme`; `Canvas`/`Field` are fixed light values and
   form controls are hard-coded white (4.3). This is the core of the
   guidance's dark-mode recipe.
3. **Implement `@starting-style`.** `transition-behavior: allow-discrete`
   exists but the at-rule does not, so the guidance's entry-animation recipe
   silently does nothing (4.1).
4. **Check the default focus indicator.** No `:focus-visible` outline rule
   was found in the UA sheet (4.4).
5. **Route the audio helper's fetch through the shared network policy.**
   It has its own libcurl setup with a different User-Agent, no TLS floor
   and no proxy settings (5.1).
6. **Retire the `with(window)` + `eval` iframe script path**, including the
   `webpackChunk` source sniff, once the native-realm path covers every
   case (5.2).
7. **Move the ~50 JavaScript snippets embedded as C strings in `src/js.c`
   into `data/js/`**, where the existing build step already syntax-checks
   scripts (5.3).
8. **Modernize the internal pages**: `dvh`, `light-dark()`, `<search>`,
   `type="url"`, `<dialog>`, no inline handler, dark-aware viewers (3.1-3.3).
9. **Fix stale documentation**: `AGENTS.md` contradicts `CLAUDE.md` on image
   decoding and the external player; packaging text and two docs still
   describe a gdk-pixbuf fallback (5.6).
10. **Drop dead compatibility surface**: `-ms-*` display values,
    `ms*` fullscreen aliases, `chrome.loadTimes()`/`chrome.csi()`, `<blink>`
    animation (4.5).

## 2. What the guidance says

Paths are relative to `skills/modern-web-guidance/guides/` in the guidance
repository unless noted.

- **Purpose** (`README.md`): steer authors from JavaScript-heavy legacy
  patterns to native platform features, with lightweight fallbacks. It lists
  250 features (140 CSS, 36 HTML/DOM, 74 JS/API) and 203 use-case guides.
- **Fallback policy** (`SKILL.md`): Baseline Widely available features are
  used without fallback; anything newer needs the guide's fallback, normally
  selected by `@supports` or a property check on a prototype. This is why
  section 4.2 matters: a browser that answers feature detection wrongly
  defeats the fallback.
- **CSS** (`css/css.md`): logical properties over physical ones; `@layer`
  and `:where()` instead of BEM; `:has()` instead of JS-managed state
  classes; `:is()`/`:where()` for forgiving fallbacks; `:not()` instead of
  override rules; `@scope` for proximity; native nesting; no global `*`
  resets; design tokens as custom properties.
- **Layout** (`css/css-layout.md`, `css/responsive-design.md`): flexbox,
  grid, subgrid, `gap`, container queries and `cq*` units, anchor
  positioning for tethered overlays, `overflow: clip`,
  `scrollbar-gutter: stable`, `overscroll-behavior`, `dvh`/`dvw` instead of
  `vh`/`vw`, `aspect-ratio`. Tables are for data, never layout
  (`accessibility/accessibility.md`).
- **HTML** (`html/html.md`): doctype, `lang`, viewport meta, landmarks,
  `<search>`, `<button type="button">`; `<dialog>` with `showModal()`,
  `closedby`, `<form method="dialog">`; `popover`; `<details name>` for
  exclusive accordions; `inert`; `fetchpriority`, `loading="lazy"`,
  `srcset`/`<picture>`; no inline event handlers; no positive `tabindex`.
- **Forms** (`forms/forms.md` and siblings): native constraints,
  `:user-invalid` instead of `:invalid`, specific `autocomplete` tokens,
  `inputmode`/`enterkeyhint`, `field-sizing`, `accent-color`, customizable
  `<select>`, `<input type="checkbox" switch>`.
- **Colour and dark mode** (`visual-design/color.md`,
  `visual-design/dark-mode.md`): `<meta name="color-scheme">` plus CSS
  `color-scheme`, `light-dark()`, `prefers-color-scheme`, forced colors,
  `oklch`/`oklab` interpolation, `color-mix()`, relative colors,
  `scrollbar-color`.
- **Typography** (`visual-design/typography.md`): `rem`/`em` not `px` for
  font size, unitless `line-height`, no fixed heights on text containers,
  `text-wrap: balance`/`pretty`, `clamp()` for fluid type.
- **Motion** (`ui-behaviors/motion.md` and siblings): scroll-driven
  animations instead of scroll listeners, view transitions,
  `@starting-style` + `transition-behavior: allow-discrete` + `overlay` for
  entry/exit, individual transform properties, `prefers-reduced-motion`
  handled per rule, `scrollend` instead of debounced `scroll`.
- **Performance** (`performance/performance.md` and siblings):
  `async`/`defer`, resource hints, `content-visibility` with
  `contain-intrinsic-size`, `scheduler.yield()`, `fetchLater()`, speculation
  rules, modern image formats.
- **Security and privacy** (`security/security.md`, `privacy/privacy.md`):
  `textContent`/`setHTML()` over `innerHTML`, Trusted Types, CSP,
  `__Host-` cookies, `Partitioned`, COOP/CORP, Fetch Metadata, passkeys,
  feature detection instead of User-Agent sniffing.
- **Out of scope for this project by its own rules**: `built-in-ai/*`
  (Prompt, Summarizer, Translator, Language Detector) and `webmcp/*`
  (`document.modelContext`). `CLAUDE.md` forbids AI-style web APIs, so these
  are listed as "absent by design" and no work is proposed.
- **What the guidance does not say**: it has no rule about `var`, arrow
  functions or other ECMAScript syntax. Its examples use `const`/`let`, but
  the ES5 style noted in 3.4 is a house-style observation, not a guidance
  violation.

## 3. Lens A: web content the browser ships

The internal pages are already in good shape: every `about:` page declares
`color-scheme`, uses custom-property tokens, flexbox/grid, `<details>`,
`<main>`/`<nav>`, `aria-label`s and `fetch`. The findings are refinements.

### 3.1 Shared stylesheet (`src/about_style.h`)

| What | Where | Why it is old | Replacement | Effort / risk |
|------|-------|---------------|-------------|---------------|
| Dark tokens duplicated in a `prefers-color-scheme` block | `src/about_style.h:21`, `:73` | Guidance: declare each token once with `light-dark()` | `--bg: light-dark(#f5f7fb, #0f1218)` etc. The engine parses `light-dark()` (`src/css_color.c:2081`) | S / low, but see 4.3 first |
| Global reset `*{box-sizing:border-box}` | `:28` | Guidance: "No global resets" | Apply to the element types that need it | S / low |
| `px` font sizes throughout | `:38`, `:47`; grep `font-size:[0-9.]*px` gives 41 in `src/net.c`, 8 in `src/history.c`, 5 in `src/about_style.h`, 4 in `src/safebrowsing.c`; `rem` count in `about_style.h` is 0 | Guidance: `rem` honours the user's font size | Convert to `rem` | S / low |
| Fixed `height:40px` on `.btn`, which contains text | `:36` | Guidance: no fixed height on text containers | `min-block-size` | S / low |
| `.btn:hover` also matches disabled buttons | `:40`, `:44` | Guidance: avoid overmatching | `.btn:hover:not(:disabled)` | S / low |
| Physical margins | `:47` (`margin:0 0 10px 20px`) | Guidance: logical properties | `margin-inline-start` | S / low |

### 3.2 Page templates

| What | Where | Why it is old | Replacement | Effort / risk |
|------|-------|---------------|-------------|---------------|
| Inline event handler | `src/net.c:4245` (`onclick="history.back()"`) | Guidance: no inline handlers; also blocks a strict CSP on the error page | `addEventListener` in a script, or a link | S / low |
| `<html>` without `lang`, no viewport meta | error page `src/net.c:4187`; view-source `src/net.c:6157`; image and text viewers `src/html.c:133`, `:297`, `:379`; `src/libnordstjernen.c:1322` | Guidance section 1 of `html/html.md` | Add both | S / low |
| Light-only viewers with hard-coded colours and no `color-scheme` | view-source `src/net.c:6161` (`background:#fff;color:#000`); document viewer `src/html.c:146`; image viewer `src/html.c:135` | Inconsistent with the `about:` pages, which all adapt | Reuse the tokens from `about_style.h` | S / low |
| `100vh` | `src/net.c:4196`, `:5215`, `:6011`, `:6028`; `src/html.c:134`, `:136` | Guidance: `dvh` | `100dvh` (engine has `dvh`, `src/css.c:3134`) | S / low |
| `role="search"` on a `<form>` | `src/net.c:5281` | Guidance: `<search>` | Wrap in `<search>` (the UA sheet already knows the element) | S / low |
| JS toggles a `typed` class from an `input` listener | `src/net.c:5336` | Guidance: style from state with `:has()` / `:placeholder-shown` | `.search:has(input:not(:placeholder-shown))` | S / low |
| URL fields as `type="text"` | `src/net.c:5548`, `:5556` | Guidance: specific input types | `type="url"` | S / low |
| Hand-built switch (hidden checkbox + span) | `src/net.c:5443-5448`, styles `:5484-5497` | Guidance `forms/switch.md` | Keep for now; revisit if the engine gains `switch` | - |
| `confirm()` for destructive action | `src/net.c:5683` | Guidance: `<dialog>` + `<form method="dialog">` | Native dialog; engine has `showModal`, `closedby` | M / low |
| Toast driven by class + `setTimeout` | `src/net.c:5506`, script `:5644` area | Guidance `ui-components/persistent-toast-notifications.md`: `popover="manual"` | Popover with `role="status"` (already present) | S / low |
| Scrollspy via a `scroll` listener | `src/net.c:5707-5715` | Guidance prefers CSS scroll markers, but `scroll-target-group` is not in the engine | Keep; or `IntersectionObserver` | - |
| Focus ring replaced by `box-shadow` after `outline:none` | `src/net.c:5482-5484`; `outline:0` at `:5236`, `src/history.c:177` | Guidance `color.md`: `box-shadow` disappears in forced colors | Keep a transparent `outline` | S / low |
| Directory listing uses a real data table | `src/net.c:4705` | Fine: it is tabular data | none | - |

### 3.3 Internal pages are not translated

`CLAUDE.md` says UI strings are translated through `src/i18n.c`. The `about:`
pages are English only: `grep -c 'ns_i18n'` gives 0 for `src/net.c`,
`src/history.c` and `src/safebrowsing.c` (110 for `src/gtk/procwindow.c`),
every page hard-codes `lang="en"`, and `src/history.c:238-243` carries its
own English weekday and month tables. Proposal: pass page strings through
`ns_i18n()` and emit the active language in `lang`. Effort M, risk low.
Unsure whether this is an intended limit; no doc says so.

### 3.4 Shipped scripts

- `data/js/polyfills.js` is 15,466 lines of ES5-style code: 2,300 `var`,
  0 `let`, 0 `const`, 0 arrow functions, 205 `for (var`, 52 `indexOf(...)`
  comparisons (grep counts on that file). The other four files in
  `data/js/` are the same. It runs only on the project's own engines, both
  of which support current syntax, so this is a maintainability choice, not
  a compatibility need. Converting 15k lines wholesale is not worth the
  risk; new code could simply use `const`/`let`.
- The file is not all polyfills. It defines whole object models (Media
  Source at `:1318`, IndexedDB at `:6244`, Cache at `:4470`, Navigation at
  `:8612`, XPath at `:14599`). The name hides that these are the
  implementation. Consider splitting by area into several `data/js/` files;
  the meson embed step (`src/meson.build:27`) already takes a list.
- Settings script (`src/net.c:5627-5716`): hand-rolled form encoder `enc()`
  at `:5629` where `URLSearchParams` exists; `on*` property handlers at
  `:5670-5705`; promise chains instead of `async`/`await`. Effort S.

## 4. Lens B: engine support

### 4.1 Support matrix

"Supported" means a code path beyond parsing was found. Evidence is one
representative line.

**Selectors**

| Feature | Status | Evidence |
|---------|--------|----------|
| `:has()`, `:is()`, `:where()`, `:not()` | supported | `src/css.c:2888`, `:2311` |
| `:nth-child(An+B of S)` | supported | `src/css.c:2468` |
| `:focus-visible`, `:focus-within` | supported | `src/css.c:2404` |
| `:user-valid` / `:user-invalid` | supported | `src/css.c:26305-26314` |
| `:modal`, `:popover-open`, `:open`, `:dir()`, `:placeholder-shown`, `:indeterminate`, `:autofill` | supported | `src/css.c:2409-2421`, `:2509` |
| `:host-context()`, `::part()`, `:state()` | recognized | `src/css.c:2226`, `:2238` (matching not checked) |
| `::details-content`, `::file-selector-button` | recognized | `src/css.c:2240` (rendering not checked) |
| `::highlight()` and the Custom Highlight API | missing | name listed at `src/css.c:2238`; no `Highlight` or `CSS.highlights` in `src/js*.c` or `polyfills.js` |
| `::view-transition-*` | recognized only | `src/css.c:2241`; no view transition implementation |
| `::scroll-marker`, `:target-current`, `::picker()`, `::checkmark` | missing | no hits |

**At-rules, functions, units**

| Feature | Status | Evidence |
|---------|--------|----------|
| `@layer`, `@scope`, `@container` (size), `@property`, `@supports`, nesting | supported | `src/css.c:23641`, `:23619`, `:23585`, `:23661`, `:23318` |
| `@position-try`, anchor positioning, `anchor-size()`, `flip-block` | supported | `src/css.c:23415`, `src/css_anchor.c` (1,389 lines) |
| `@starting-style` | **missing** | not among the at-rules handled at `src/css.c:23303-23755`; grep `starting` finds nothing in the CSS sources |
| `@container style()` | not verified | no `style_query`-like symbol found |
| `@container scroll-state()`, `@function`, `if()`, `at-rule()` | missing | no hits |
| `@view-transition` | missing | no hits |
| `color-mix()`, relative colors, `oklch`/`oklab`, `contrast-color()`, `light-dark()` | supported, see 4.3 for `light-dark()` | `src/css_color.c:2001`, `:2081`; `src/css.c:6108` |
| `min()`/`max()`/`clamp()`, trig, `round()`, `abs()`/`sign()`, `progress()`, `sibling-index()`/`sibling-count()` | supported | `src/css.c:3550-3555`, `:7533` |
| `calc-size()` | parse only | single hit at `src/css_prop_syntax.c:190` |
| `dvh`/`svh`/`lvh`, `cq*`, `lh`/`rlh`, `cap` | supported | `src/css.c:3134-3177` |
| `env()` | parsed | `src/css.c:3527`; no `safe-area-inset` name anywhere |
| `image-set()`, `linear()` easing | supported | `src/css.c:7363`, `:8664` |

**Properties**

| Feature | Status | Evidence |
|---------|--------|----------|
| Flexbox, grid, subgrid, `gap`, `aspect-ratio`, logical properties | supported | `src/css.c:5540`, `:10928` |
| Individual `translate`/`rotate`/`scale` | supported | `src/css.c:521-523` |
| Scroll-driven animations (`animation-timeline`, `scroll-timeline`, `view-timeline`, `animation-range`) | supported | `src/anim.c:919`, `:1980` |
| `transition-behavior: allow-discrete`, `overlay` | supported | `src/anim.c:528`, `src/css.c:14580` |
| Scroll snap | supported | `src/layout.c:21405` |
| `accent-color`, `scrollbar-color`, `scrollbar-width` | supported | `src/paint.c:5627`, `:11386`, `:11379` |
| `content-visibility` | partial: `hidden` implemented, `auto` only as containment | `src/layout.c:570`, `:9232` |
| `text-wrap: balance` | supported | `src/layout.c:12242` |
| `text-wrap: pretty` | parse only | only in the grammar string at `src/css.c:668` |
| `hyphens`, `font-size-adjust`, masks, `clip-path`, blend modes (`mix-blend-mode`), `backdrop-filter`, `overflow-clip-margin`, `line-clamp` | supported | `src/paint.c:3348`, `src/css.c:13590`, `:15272` |
| `color-scheme` | partial | see 4.3 |
| `field-sizing`, `interpolate-size`, `scrollbar-gutter`, `scroll-behavior`, `overscroll-behavior`, `touch-action`, `resize`, `background-blend-mode`, `forced-color-adjust`, `text-decoration-skip-ink`, `view-transition-name`, `view-transition-class` | **parse only** | see 4.2 |
| `text-box`, `corner-shape`, `border-shape`, `reading-flow`, grid lanes/masonry, `scroll-initial-target`, `scroll-target-group`, `text-fit`, `interactivity` | missing | no hits in `src/css*.c`, `src/layout.c`, `src/paint.c` |

**HTML, DOM, JS**

| Feature | Status | Evidence |
|---------|--------|----------|
| `<dialog>`, `showModal()`, `requestClose()`, `closedby` | supported | `src/js.c:49705`, `:24019`, `:49446` |
| Popover incl. `popover="hint"`, `popovertarget` | supported | `src/js.c:49066`, `:48364` |
| Invoker commands (`command`/`commandfor`) | supported | `src/js.c:49301`, `:49905` |
| Interest invokers (`interestfor`) | missing | only the name in the shape list, `src/js.c:61221` |
| `<details name>` exclusive groups | supported | `src/js.c:48243` |
| `hidden="until-found"`, `beforematch` | supported | `src/css.c:32244`, `src/js.c:60290` |
| `inert`, `moveBefore()` | supported | `src/dom.c:2710`, `src/js.c:56258` |
| Declarative shadow DOM, `attachInternals`, form-associated custom elements | supported (partly in JS) | `src/dom.c:2854`, `data/js/polyfills.js:14835-14964` |
| `setHTMLUnsafe()` / `setHTML()` | partial: sanitizing done in script, no `Sanitizer` constructor | `src/js.c:9148`, `data/js/polyfills.js:14724` |
| `loading="lazy"` | supported | `src/engine.c:431` |
| `fetchpriority` | reflected only; request urgency is set from destination, not the attribute | `src/js.c:56397`, `src/net.c:6818-6823` |
| Speculation rules, `fetchLater()`, `blocking="render"`, `<link rel="expect">` | missing | no hits |
| View Transitions API | missing | `startViewTransition` appears only in name lists (`src/js.c:60961`, `:61090`) |
| `scheduler.postTask()` / `yield()` | supported in JS | `data/js/polyfills.js:8809`, `:8837` |
| Intersection/Resize/Mutation observers, `AbortController`, Pointer Events, `scrollend`, Navigation API, Trusted Types, `structuredClone` | supported | `src/js.c:32680`, `:33433`, `:31611`; `polyfills.js:8612`, `:8730` |
| `scrollsnapchange` | name only | `src/js.c:60947` |
| Temporal, `Intl.DurationFormat` | engine-provided (`Intl.DurationFormat` found only in the quickjs-ng fork, `src/quickjs/quickjs-intl-duration.c`) | not checked on the default engine |
| Web Crypto incl. Ed25519/X25519, WebAuthn | supported | `src/webcrypto.c`, `polyfills.js:15249` |
| `Partitioned` cookies | missing | no case-insensitive hit in `src/cookies.c` or `src/net.c` |
| `Permissions-Policy`, `Clear-Site-Data` response headers | missing | no hits |
| `<input type="checkbox" switch>`, customizable `<select>`, `focusgroup`, `<meta name="text-scale">`, WebOTP, FedCM, `beforeinstallprompt` | missing | no hits |
| Built-in AI, WebMCP | absent by design | `CLAUDE.md` |

### 4.2 Parse-only properties answer `@supports` incorrectly

`kPropDefs` (`src/css.c:582-745`) registers properties with a grammar so
they parse, serialize and appear in computed style. For the properties
below, the enum is referenced only by that table and `src/css.h`; no layout,
paint, animation or binding code reads it. Method: for each table entry,
count whole-word uses of its `NS_CSS_*` identifier across first-party
sources; these have exactly two.

`field-sizing` (`:719`), `interpolate-size` (`:721`), `scrollbar-gutter`
(`:675`), `scroll-behavior` (`:599`), `touch-action` (`:601`), `resize`
(`:646`), `background-blend-mode` (`:679`), `forced-color-adjust` (`:671`),
`text-decoration-skip-ink` (`:589`), `view-transition-name` (`:723`),
`view-transition-class` (`:725`), plus `overscroll-behavior-x/-y` (three
uses, all in `src/css.c`).

Why it matters: the guidance tells authors to gate these on `@supports` and
ship a fallback otherwise (for example a JS auto-grow for textareas when
`field-sizing: content` is unsupported, or a JS height animation when
`interpolate-size` is). A property that parses makes `@supports` true, so
the page takes the native branch and gets nothing.

Proposal, per property: either implement it, or make
`CSS.supports()`/`@supports` report false while keeping the declaration
parseable for CSSOM round-tripping. Reasonable first implementations:
`scrollbar-gutter: stable` (reserve the scrollbar width in layout),
`field-sizing: content` (size inputs to content), `scroll-behavior: smooth`.
Effort S each for the detection change, M each to implement. Risk: WPT
subtests that only check parsing may regress if support is withdrawn; rerun
`scripts/wpt-score.sh` for `css`.

Uncertain: a property could be consumed through a string lookup the count
does not see. The properties were also searched by name across first-party
sources and only table, serialization-list and shorthand hits were found,
but this was not confirmed by running the browser.

### 4.3 `color-scheme` does not drive colours

- `light-dark()` picks its branch from the global preference
  (`src/css_color.c:2098`, context set at `:2217` from
  `ns_css_get_color_scheme()`), not from the element's used
  `color-scheme`. A page that uses `light-dark()` without opting into dark
  gets dark values on a dark desktop, and `color-scheme: dark` on a subtree
  has no effect.
- System colors are fixed light values: `canvas` 255,255,255, `canvastext`
  0,0,0, `field` 255,255,255, `buttonface` 240,240,240
  (`src/css.c:1896-1905`).
- The UA sheet paints controls with literals: `background-color: #ffffff`
  for inputs (`src/css.c:32186`), `#e6e6e6` for buttons (`:32163`).
- `NS_CSS_COLOR_SCHEME` is read only for canonical serialization
  (`src/css.c:14378`).

Result: `<meta name="color-scheme" content="light dark">`, the first step of
the guidance's dark-mode recipe and something this project's own `about:`
pages declare, does not give a dark canvas or dark controls. Proposal:
compute a used colour scheme per element (inherited, intersected with the
preference), resolve system colors and `light-dark()` from it, and switch
the UA sheet's control colours to `Field`/`FieldText`/`ButtonFace`/
`ButtonBorder`. Effort M-L, risk medium (every page's default colours pass
through this). Verify with a page that sets only the meta tag, under both
`color_scheme = light` and `dark` in the config.

### 4.4 Default focus indicator

No rule containing `:focus` or `focus-visible` exists in the UA sheet
(`sed -n 32060,32240p src/css.c | grep -c 'focus-visible\|backdrop\|:focus'`
gives 0), and `src/paint.c` only special-cases focused text fields
(`NS_INLINE_INPUT_FIELD_FOCUSED`). If nothing else draws a ring, keyboard
users get no focus indication on links and buttons unless the page styles
it. The same grep shows no `::backdrop` rule for modal dialogs. Proposal:
add `:focus-visible { outline: auto }` and a `dialog::backdrop` rule to
`kUa`. Effort S. **Not verified at runtime**; check by tabbing through a
plain page first.

### 4.5 Legacy surface carried by the engine

**Keep: required for real-web compatibility**

- `-webkit-` aliases from the Compatibility Standard: `-webkit-box`,
  `-webkit-line-clamp` (the guidance itself still recommends this triad),
  `-webkit-text-fill-color`, `-webkit-text-stroke`, `-webkit-mask*`,
  `-webkit-appearance`, `-webkit-gradient`, `@-webkit-keyframes`
  (`src/css.c:23437`), `-webkit-device-pixel-ratio`. Grep
  `"-webkit-` in `src/css.c` gives 56 lines.
- Presentational attributes and obsolete elements the HTML standard still
  specifies rendering for: `bgcolor`, `align`, `<center>`, `<font>`,
  `<marquee>`, `<nobr>`, `br[clear]` (`src/css.c:32255-32265`, `:33663`).
- Quirks-mode sheet (`src/css.c:32267-32277`).
- `document.write`, `XMLHttpRequest`, `document.all`, `createEvent`/
  `initEvent`, `keyCode`/`charCode`, `cancelBubble`, `returnValue`,
  `webkitMatchesSelector`, `webkit`-prefixed fullscreen, `webkitURL`,
  `WebKitCSSMatrix`, `navigator.javaEnabled()`/`taintEnabled()` (both still
  in the HTML standard).

**Candidates to drop: no current engine needs them**

| What | Where | Note |
|------|-------|------|
| `display: -ms-flexbox`, `-ms-inline-flexbox`, `-ms-grid`, `-webkit-grid` | `src/css.c:10604-10608` | IE10-era values. Authors always pair them with the standard value. `-ms-grid` also has different semantics from modern grid, so honouring it alone is wrong |
| `::-ms-input-placeholder`, `::-moz-placeholder` | `src/css.c:2874-2876` | Paired with `::placeholder` everywhere |
| `msRequestFullscreen`, `msExitFullscreen`, `msFullscreenElement` | `src/js.c:56136`, `:66234`, `:66245` | IE11 / EdgeHTML only |
| `mozRequestFullScreen`, `mozCancelFullScreen`, `mozFullScreenElement`, `mozFullScreen` | `src/js.c:56135`, `:66233`, `:66244`, `:66247` | Odd for a browser whose default identity is Chrome; keep only under the Firefox compat mode if at all |
| `chrome.loadTimes()`, `chrome.csi()` | `src/js.c:62128-62137` | Deprecated by Chrome in 2018. Decide whether `window.chrome` is needed at all |
| `<blink>` animation | `src/css.c:32257-32258` | The HTML standard gives `blink` no rendering; no current browser blinks |
| `-moz-appearance`, `-moz-user-select`, `-moz-available` | `src/css.c:15929`, `:19224`, `:15017` | Low value; harmless. Drop only for tidiness |
| Privacy Sandbox member names in the interface shape lists (`browsingTopics`, `attributionSrc`) | `src/js.c:60911`, `:61203`, `:61251`, `:61255` | Names only, nothing implemented; remove so the lists match what exists |

Effort S for all, risk low. Verify with `scripts/wpt-score.sh` on `css` and
`fullscreen`, and by loading a few mainstream sites.

**Observation, no change proposed**: the default User-Agent claims Chrome
(`src/net.h:24-61`) and `navigator.vendor` is "Google Inc."
(`src/js.c:62421`). The guidance asks sites to feature-detect, but sites
that sniff will assume the full feature set of that Chrome version, which
raises the cost of every "missing" row above.

## 5. Lens C: old patterns in the C code and build

Overall the C is in better shape than the brief anticipated. Counts over the
227 first-party files: `strcat`, `sprintf`, `strncpy`, `gets`, `alloca`,
`#if 0`, `TODO`/`FIXME` markers: all 0. No deprecated GTK widgets
(`GtkTreeView`, `GtkDialog`, `GtkFileChooser`, `gtk_widget_show`, etc.: 0;
the shell uses `GtkFileDialog` and `GtkAlertDialog`). No low-level OpenSSL
(`RSA_*`, `EC_KEY_*`, `SHA256_Init`: 0). No removed FFmpeg API. No
`<quickjs.h>` or Pango include outside `src/ns_quickjs.h:9` and
`src/ns_pango.h:15`, which are the designated wrappers. No hard-coded site
hostnames beyond the search-engine list. Hardening flags are present
(`meson.build:42-59`).

### 5.1 The audio helper has its own network stack settings

`src/audio/main.c:1366-1388` builds a libcurl request by hand. Compared with
the shared `ns_net_apply_curl_tls()` (`src/net.c:2639`, used by
`eventsource.c:381`, `ws.c:453`, `safebrowsing_google.c:172`), it:

- sends `User-Agent: Nordstjernen-Audio` (`:1379`), so media requests are
  distinguishable from page requests and reveal the real browser behind the
  Chrome User-Agent;
- sets no `CURLOPT_SSLVERSION`, cipher list or curves, so it does not share
  the TLS 1.2 floor at `src/net.c:2644`;
- sets no proxy options, so the user's proxy configuration
  (`docs/Proxy.md`) is presumably not applied (no `CURLOPT_PROXY` in the
  file; libcurl's environment variables would still apply);
- reads `CURL_CA_BUNDLE`/`SSL_CERT_FILE` with raw `getenv` (`:1385`).

Proposal: have the shell pass the User-Agent and proxy to the helper and
share one small TLS-setup function. Effort M, risk medium (helper is a
separate process without the engine library). Verify with `--debug=net` and
a proxy while playing an `.mp3`.

### 5.2 Two iframe script paths; the older one rewrites source text

`src/js.c:72124-72130` wraps concatenated iframe scripts as
`(function(window,self,...){ with(window){ ... } })`. To make top-level
`var`s visible on the frame's window it scans the script text for names
(`ns_js_iframe_collect_var_names`, `src/js.c:71464`) and defines accessors
that call `eval` (`:71583-71586`). The scan is skipped when the first 4 KB
contain `webpackChunk` or `webpackJsonp`
(`ns_js_iframe_source_is_webpack_chunk`, `:71559`). A second path creates a
real context per frame (`JS_NewContext`, `src/js.c:55394`); the wrapper runs
only when `ns_iframe_parent_realm_is_native()` is false (`:72123`).

Why it is old: `with` forces sloppy mode, so strict-mode scripts, modules
and top-level `let`/`class` behave differently from a real global scope;
text scanning of JavaScript is fragile; and the bundler-name check is a
library-specific special case of the kind `CLAUDE.md` rules out. Proposal:
establish when the non-native path is still reached, close those gaps, then
delete the wrapper, the scanner, the webpack check and the prototype
snapshot/cleanup helpers (`:71589-71640`). Effort L, risk high (frames are
everywhere). Verify with the `html/browsers` and `html/semantics/embedded-content`
WPT areas. **Unsure** how often the legacy path runs today; this was not
traced.

Related: `__shady_*` and `__shady_native_*` properties are installed on DOM
prototypes for the Polymer ShadyDOM polyfill (`src/js.c:63219-63220`,
exempted at `:61005` and `:61388`). With native shadow DOM a page should not
load ShadyDOM at all; check whether this is still needed.

### 5.3 JavaScript embedded in C string literals

`src/js.c` has 62 `JS_Eval` call sites and about 50 named embedded scripts
(`"<node-shapes>"`, `"<event-target>"`, `"<iframe-scope>"` and so on; grep
`'"<[a-z0-9-]+>"'`), with 647 C-string lines containing `function(` or
`var x =`. They get no syntax check until runtime, cannot be linted, and
need C escaping. The project already has the fix:
`scripts/embed-text.py` embeds `data/js/*.js`, and
`scripts/verify-polyfills.py` compiles `polyfills.js` with `qjsc` at build
time (`src/meson.build:12-28`). Move the larger snippets into `data/js/`
and run the same check on them. Effort M (mechanical), risk low. It also
shrinks the 74k-line file, which bears on the "readable by a single human"
constraint.

### 5.4 Engine state stored as `data-nd-*` attributes

42 distinct `data-nd-*` attribute names hold engine state on DOM nodes
(value dirtiness, popover and modal open state, frame URLs, shadow-root
flags, media state). Grep `data-nd-[a-z0-9-]+` over first-party sources.
They are hidden from script by `ns_attr_name_is_internal()`
(`src/dom.c:1339`), which has to be called at about 28 separate sites in
`src/js.c`, `src/dom.c`, `src/xml.c` and `src/html_lexbor.c`. The UA sheet
selects on them (`src/css.c:32250`, `[data-nd-popover-open]`).
`src/css.c` contains no call to the filter, so author selectors such as
`[data-nd-modal]` appear able to observe this state (not verified at
runtime), and each new exposure path needs its own filter call.

Proposal: move boolean state to node flags (`ns_node.flags` already exists,
`NS_NODE_QUIRKS`) and string state to a side struct, and match UA rules
through internal pseudo-classes. Do it incrementally, starting with the
selector-visible ones (`popover-open`, `modal`, `vdirty`, `autofill`).
Effort L overall, S-M per attribute; risk medium.

### 5.5 Dated C idioms

| What | Where | Note | Replacement | Effort / risk |
|------|-------|------|-------------|---------------|
| `strtok` (non-reentrant) in a multithreaded process | `src/rproc_http.c:2351-2352`, `:2381-2382` | Parses `/proc/<pid>/stat` | `strtok_r` or `g_strsplit` | S / low |
| `localtime` (non-reentrant) | `src/engine.c:2612` | PDF date | `g_date_time_new_now_local` | S / low |
| `signal(SIGPIPE, SIG_IGN)` | `src/audio/main.c:2041`, `src/renderer_http.c:168`, `:217`, `src/rproc_http.c:214`, `:547` | `sigaction` exists once elsewhere | `sigaction`, or one shared helper | S / low |
| `atoi`/`atol` (35 + 5 uses in 13 files) | e.g. `src/net.c:5808-5832` | `src/net.c:5808` casts `atoi()` of a form field straight to `ns_cookie_policy` with no range check | `g_ascii_strtoll` with validation; `src/config.c` already has `parse_enum` | S / low |
| `atof` (locale dependent) | `src/audio/main.c:2117`, `:2120`, `:2123` | Seek/volume would misparse under a comma-decimal locale if `setlocale` is called | `g_ascii_strtod` | S / low |
| `strcpy` (9 uses, 4 files) | `src/css_anchor.c:165`, `:270`, `:621`; `src/css_color.c:651`, `:1158`; `src/css_shape.c:247`, `:255`, `:613`; `src/svg.c:493` | All copy short literals into fixed fields, or are length-checked first (`src/svg.c:489`). Low severity | `g_strlcpy` for uniformity | S / none |
| Raw `getenv` (13 uses, 5 files) beside `g_getenv` (93 uses) | `src/js.c:54289`, `src/net.c:6525`, `:7201`, `:7256`, `src/media_os_*.c` | Inconsistent | `g_getenv` | S / none |
| Deprecated `g_thread_create_full` | `src/js.c:30905-30909` | Deliberate: the only GLib call that sets a stack size; wrapped in `G_GNUC_BEGIN_IGNORE_DEPRECATIONS` | Keep, or use `pthread_attr_setstacksize` / `_beginthreadex` behind one helper | M / medium |

### 5.6 Stale documentation and leftovers

| What | Where | Contradicts |
|------|-------|-------------|
| `AGENTS.md` says image decoding ends in a "GDK-Pixbuf fallback (TIFF and other loader-backed formats)" | `AGENTS.md:162-163` | `CLAUDE.md` ("Nothing follows"); `src/image*.c` has no gdk-pixbuf call |
| `AGENTS.md` says macOS "falls back to the external player for WebM" | `AGENTS.md:255` | `CLAUDE.md`: the GTK shell does not launch an external player |
| `AGENTS.md` and `CLAUDE.md` are two hand-maintained copies that have drifted (WebGL, WebGPU and media sections differ) | `diff AGENTS.md CLAUDE.md` | Keep one source and generate or include the other |
| "GDK-Pixbuf image fallback" row | `docs/tab-isolation.md:85` | same |
| Packaging comment "GDK-PixBuf loader cache + loader DLLs (image decode for `<img>`)" | `scripts/pack-windows.sh:134` | The loaders are now needed only for GTK's own icons (`src/gtk/icons.c:266`) |
| Requirements text "GTK 4.6+" | `scripts/pack-linux.sh:215` | Floor is 4.14 (`meson.build:125`) |
| Comments about an "external-player fallback" | `meson.build:436-438`, `docs/Linux.md:54` | No such launch path in the shell |
| `Recommends: mpv` | `nordstjernen.spec:54`; described in `docs/media.md:226`, `docs/opensuse.md:127` | Nothing in the shell invokes it; decide whether the recommendation still earns its place |
| Compatibility snapshots lag the code | `docs/CSS-compatibility.md:15` (1.0.23), `docs/HTML-compatibility.md:19` (1.0.16); tree is 1.0.47-dev | Section 4 rows (for example `@starting-style`, parse-only properties) should be folded in |
| Environment variable spelled two ways | `NS_MEDIA_NO_HW_DECODER` in `src/media_os_gst.c:118` and `docs/media.md:137`; `NS_MEDIA_NO_HW_DECODE` in `src/media_os_mf.c:315` | The documented name does nothing on Windows |
| Old `ND_` macro prefix | `ND_HAVE_WEBGPU` (8 uses, set at `meson.build:349`), `ND_WASM_LOG` | Everything else is `NS_` |

All effort S, risk none.

### 5.7 Duplicated helpers

| What | Where |
|------|-------|
| Three `calc()` parsers | `src/css.c:3520-3699`; `src/css_color.c:962-1027`; `src/css_shape.c:152-217` |
| Four number formatters | `src/css_anchor.c:603`; `src/css_color.c:1163`; `src/css_filter.c:207`; `src/css_shape.c:268` |
| Same-named static helpers in two files | `overflow_kw_clips` (`src/layout.c:8934`, `src/paint.c:277`); `box_overflow_clip_axes` (`src/layout.c:19900`, `src/paint.c:10666`); `value_is_wide_keyword` (`src/anim.c:1410`, `src/layout.c:20443`); `next_element_sibling` (`src/css.c:25787`, `src/js.c:40015`); `parse_angle_deg` (`src/css.c:7901`, `src/css_shape.c:879`); `bytes_are_ogg` (`src/audio/main.c:542`, `src/video_decode.c:142`) |
| Second URL parser | `g_uri_parse` in `src/history.c:252`, while `CLAUDE.md` routes URL work through `ns_url_*` |

Proposal: one shared number formatter and one shared overflow helper in a
header are quick wins (S). Unifying the `calc()` parsers is M-L and risky,
because the three produce different result types; do it only when one of
them next needs a fix.

### 5.8 Project-rule checks

- **Comments policy**: 10 block comments past the file header, in 4 files:
  `src/audio/main.c:57`, `:74`, `:1083`, `:1627`; `src/media_os_mf.c:38`,
  `:304`, `:400`; `src/css.c:37278`, `:37818`; `src/video.c:717`. No
  trailing or `//` comments found. Effort S to remove.
- **No test suite**: `data/render-tests/` (71 files), `data/fixtures/` and
  `data/baseline/` with `scripts/render-tests.sh` form a regression suite
  in all but name. Either the rule or the directories should be described
  accurately.
- **File size**: `src/js.c` 74,186 lines, `src/css.c` 39,161,
  `src/layout.c` 22,740. These three are 48% of the first-party code. 5.3
  is the cheapest first cut; the UA sheet and property tables are the
  natural next extractions from `css.c`.
- **CI**: most workflows reference actions by mutable tag
  (`actions/checkout@v7`), one pins a commit
  (`.github/workflows/sysroot-bump.yml`). Pick one policy.
- **TLS ciphers**: the list at `src/net.c:2645-2650` still offers CBC-SHA1
  and RSA key-exchange suites (`AES128-SHA`, `AES256-SHA`,
  `AES128-GCM-SHA256`). They track what Chrome offers, so dropping them
  changes the TLS fingerprint and may break old servers. Flagged for a
  decision, not recommended either way.

## 6. Proposed work plan

Ordered by value over effort. Every item is verified by building
(`meson compile -C builddir`) and running the browser, per the project's
definition of done.

| # | Item | Files | Size | Risk | How to verify |
|---|------|-------|------|------|---------------|
| 1 | Fix stale docs and names (5.6); remove 10 comments (5.8) | `AGENTS.md`, `docs/tab-isolation.md`, `docs/CSS-compatibility.md`, `scripts/pack-linux.sh`, `scripts/pack-windows.sh`, `meson.build`, `src/media_os_mf.c`, `src/audio/main.c`, `src/css.c`, `src/video.c` | S | none | Build; on Windows set `NS_MEDIA_NO_HW_DECODER=1` and play H.264 |
| 2 | Internal page fixes (3.1, 3.2) | `src/about_style.h`, `src/net.c`, `src/history.c`, `src/html.c`, `src/safebrowsing.c` | S | low | Open `about:start`, `about:settings`, `about:history`, a failing URL, `view-source:`, an image URL, in light and dark |
| 3 | Small C idiom fixes (5.5), including the `cookie_policy` range check | `src/rproc_http.c`, `src/engine.c`, `src/net.c`, `src/audio/main.c` | S | low | Change each setting in `about:settings`; print to PDF; seek in an audio file |
| 4 | UA default focus ring and `dialog::backdrop` (4.4), after confirming the gap | `src/css.c` (`kUa`) | S | low | Tab through a plain page; open a modal dialog |
| 5 | Drop dead compat surface (4.5) | `src/css.c`, `src/js.c` | S | low | `scripts/wpt-score.sh` for `css` and `fullscreen`; load several mainstream sites |
| 6 | Correct `@supports` for parse-only properties (4.2); implement `scrollbar-gutter`, `field-sizing`, `scroll-behavior` first | `src/css.c`, `src/layout.c`, `src/paint.c` | S each to gate, M each to implement | medium | Test page with `@supports` branches per property; `scripts/wpt-score.sh css` |
| 7 | `@starting-style` (4.1) | `src/css.c`, `src/anim.c` | M | medium | The popover and dialog entry-animation examples from `ui-behaviors/animate-element-entry-exit.md` |
| 8 | Used `color-scheme`, scheme-aware system colors and `light-dark()` (4.3) | `src/css.c`, `src/css_color.c`, `src/paint.c` | M-L | medium | Page with only the `color-scheme` meta; `about:` pages; form controls in both schemes |
| 9 | Audio helper network policy (5.1) | `src/audio/main.c`, `src/gtk/procview.c`, `src/net.c` | M | medium | `--debug=net` plus a proxy while playing `.mp3`/`.opus` |
| 10 | Move embedded JS out of `src/js.c` (5.3) | `src/js.c`, `data/js/*.js`, `src/meson.build`, `scripts/verify-polyfills.py` | M | low | Build (syntax check runs at build time); smoke-launch; `scripts/wpt-fast.sh` |
| 11 | Shared number-format and overflow helpers (5.7) | `src/css_color.c`, `src/css_filter.c`, `src/css_shape.c`, `src/css_anchor.c`, `src/layout.c`, `src/paint.c` | S | low | `data/render-tests/calc-serialization.html`, `filters.html` |
| 12 | Translate the `about:` pages (3.3) | `src/net.c`, `src/history.c`, `src/safebrowsing.c`, `data/i18n/*.lang` | M | low | Launch with a non-English `LANG` |
| 13 | `fetchpriority` to request urgency; `Partitioned` cookies | `src/net.c`, `src/engine.c`, `src/cookies.c` | M | low | `--debug=net` on a page with a `fetchpriority="high"` image; a `Set-Cookie` with `Partitioned` |
| 14 | Replace `data-nd-*` state with node flags, selector-visible ones first (5.4) | `src/dom.c`, `src/css.c`, `src/js.c` | L | medium | Popover, dialog and form-validation pages; `scripts/wpt-score.sh` for `html/semantics` |
| 15 | Remove the legacy iframe script path (5.2) | `src/js.c` | L | high | Trace when the path runs first; then WPT `html/browsers`, sites with many frames |
| 16 | Larger features, in guidance priority order: View Transitions, Custom Highlight API, `text-wrap: pretty`, `content-visibility: auto` skipping, `Sanitizer` | `src/css.c`, `src/layout.c`, `src/paint.c`, `src/js.c`, `src/anim.c` | L each | medium | The matching guidance example pages |

Not proposed: built-in AI and WebMCP (ruled out by `CLAUDE.md`); PWA
install, WebOTP and FedCM (need product decisions and services outside the
engine); rewriting `polyfills.js` to newer syntax (large churn, no user
benefit).
