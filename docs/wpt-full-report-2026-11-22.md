# Full WPT run — 2026-11-22

The second full run, one day after
[the first](wpt-full-report-2026-11-21.md). The test set and driver are
unchanged: every testharness.js URL in the WPT `MANIFEST.json` (40,472 URLs,
all variants), run through the headless `--wpt-batch` driver with
`scripts/wpt-full.py`.

| | |
|---|---|
| Browser | `main` @ `4f38d6c3` (yesterday `024f6590`), Windows 11 / MSYS2 UCRT64 build |
| WPT checkout | `web-platform-tests/wpt` @ `9544d3086` (2026-11-19), same as yesterday |
| Server | `./wpt serve`, `browser_host: localhost`, OpenSSL-generated certs for `localhost` |
| Timeouts | 10 s per test, 30 s for `timeout=long` |
| Parallelism | 48 batch browsers for the first 21,816 files, 24 for the rest (see below) |
| Sum of per-test time | 26.8 h (yesterday 30.2 h) |
| Raw results | `builddir/wpt-full-2026-11-22.jsonl` |

The run was interrupted once: at 21,816 files the system ran low on memory
and the runner and server were stopped. It was resumed from the same results
file with 24 jobs, which skips the files already done.

## Headline

| Metric | 2026-11-21 | 2026-11-22 | Change |
|---|---|---|---|
| Files fully passing | 11,896 | **14,149** | +2,253 |
| File-weighted score (mean per-file pass fraction) | 37.2 % | **44.4 %** | +7.2 pts |
| File-weighted score over measured files | 43.7 % (34,464) | **46.6 %** (38,583) | +2.9 pts |
| Subtests passing | 621,546 / 2,058,128 (30.2 %) | **1,638,986 / 2,157,874 (76.0 %)** | +1,017,440 |
| Subtests passing, excluding `encoding/` | 519,609 / 962,575 (54.0 %) | 541,177 / 1,005,505 (53.8 %) | +21,568 |

3,799 files improved and 104 got worse. Rerunning those 104 alone showed that
about 40 % were noise; the rest are listed under **Real regressions**.

The jump in passing subtests comes almost entirely from `encoding/`. PR #1254
gave documents their real `characterSet`, decoding that tolerates bad bytes,
and URL queries encoded in the document's encoding. That turned the 547
legacy-CJK `*-form-*` files from timeouts into passes: `encoding/` went from
101,937 to 1,097,809 passing subtests. Excluding `encoding/`, the subtest
ratio is flat because 43,000 subtests became measurable for the first time.

Harness outcome per file:

| Harness | 2026-11-21 | 2026-11-22 |
|---|---|---|
| OK | 30,151 | 33,886 |
| TIMEOUT | 4,994 | 4,398 |
| ERROR | 1,516 | 2,140 |
| NORESULT | 3,804 | 43 |
| CRASH | 7 | 5 |

The 3,731 HTTPS files that ended without a result yesterday, after the TLS
connection closed early, were measured this time. No browser change since
yesterday touches TLS, so I attribute this to the server and certificate
setup, but I have not verified the cause. Most of the new ERROR files are
these newly measured HTTPS files, for example `speculation-rules` with 394
errors.

## Largest improvements by area

| Area | Files | Score then → now | Newly fully passing | Subtests then → now |
|---|---|---|---|---|
| encoding | 1,343 | 12.6 → **92.1 %** | 872 | 101,937 → 1,097,809 |
| html | 9,211 | 52.7 → 58.4 % | 517 | 131,532 → 138,364 |
| mixed-content | 388 | 0.3 → 70.2 % | 178 | 3 → 1,580 |
| referrer-policy | 1,482 | 41.8 → 57.0 % | 169 | 3,169 → 5,390 |
| fetch | 948 | 35.5 → 47.8 % | 73 | 4,707 → 6,387 |
| content-security-policy | 949 | 33.4 → 45.1 % | 67 | 1,154 → 2,067 |
| IndexedDB | 820 | 43.5 → 56.6 % | 76 | 2,475 → 3,218 |
| upgrade-insecure-requests | 194 | 0.0 → 39.0 % | 69 | 0 → 390 |
| streams | 307 | 65.6 → 86.8 % | 58 | 3,867 → 5,149 |
| service-workers | 332 | 6.9 → 26.2 % | 34 | 298 → 1,093 |
| encoding-detection | 75 | 0.0 → 38.7 % | 29 | 0 → 29 |
| wasm | 538 | 33.4 → 38.2 % | 19 | 34,125 → 34,367 |

Most of the gains outside `encoding/`, `html` and `IndexedDB` come from HTTPS
files measured for the first time, not from engine changes.

- **`html`:** gains from query encoding (`html/infrastructure/urls`), forms
  and `document.open()`.
- **`IndexedDB`:** gains from the origin-sized database limit, and
  yesterday's seven interleaved-cursor crashes are gone.

## Crashes: an access violation in module scripts

All five crash files, plus one that showed no result in the full run, crash
again when rerun alone. Each exits with `0xC0000005`, an access violation,
less than a second after load:

- `/fetch/metadata/generated/script-module-import-static.sub.html` (and `.https`)
- `/fetch/metadata/generated/script-module-import-dynamic.sub.html` (and `.https`)
- `/import-maps/acquiring/dynamic-import.html` (no result in the full run, crash on rerun)
- `/resource-timing/render-blocking-status-script.html`

All six load module scripts, statically or via `import()`. Yesterday they ran,
for example 5 of 19 subtests passing in `script-module-import-static`, so this
is a regression in the module loader since `024f6590`. It should be
investigated before anything else, because it may be a memory-safety bug.

## Real regressions

Rerunning the 104 files that got worse, the 43 with no result and the 5
crashes, with 12 jobs:

| Outcome on rerun | Files |
|---|---|
| Back to yesterday's score or better (noise) | 35 of the 87 regressed, 42 of the 43 with no result |
| Still worse than yesterday | 52 regressed (46 after the PannerNode files below), plus the 6 module crashes above |

The noise mostly came from the first, memory-starved half of the run. Twenty
FileAPI worker files ended there with no result after 26 s, and all of them
pass when run alone. The regressions that reproduce, grouped:

| Group | Files | Example |
|---|---|---|
| Query encoding | 5 | `query-encoding/location.sub.html?encoding=windows-1252` 4 → 0 / 5, `lone-surrogates.sub.html?encoding=windows-1252`, `utf-16{le,be}.html?include=loading` — the frame-`location` cases left open by PR #1254 |
| Text module scripts | 3 | `text-module/ignore-content-type.any{,.worker,.sharedworker}.html` 102 → 92 / 106 |
| Fetch metadata | 2 | `script-text-module-import-static.sub.html` 5 → 4 / 18, `serviceworker.https.sub.html` 1 → 0 / 10 |
| Reporting API / CSP `report-to` | 7 | `content-security-policy/reporting-api/*`, `reporting/document-reporting-named-endpoints` — one subtest each |
| IntersectionObserver in iframes | 4 | `root-margin-in-same-origin-iframe`, `offset-iframe-same-origin`, `transformed-iframe-002-same-origin`, `same-document-root` |
| Canvas `filter.layers.blur.exceptions` | 3 | element, offscreen, offscreen worker |
| zstd `big-window-zstd-body` | 3 | window, worker, sharedworker |
| Event activation | 1 | `dom/events/Event-dispatch-single-activation-behavior.html` 132 → 111 / 132 |
| CSS selector invalidation | 2 | `not-001.html`, `not-pseudo-containing-complex-in-has.html` 428 → 414 |
| Charset | 1 | `html/syntax/charset/meta-commented-out.html` — likely PR #1254's charset handling |
| cookieStore creation URL | 2 | `cookieStore_get{,All}_set_creation_url.https.any.html` |
| `document.open()` while navigating | 1 | `abort-while-navigating.window.html` 2 → 0 / 9 — likely `9bfbd610` |
| Other single files | 12 | `meta-csp-img-src-none` (speculative parsing), `meta-color-scheme-remove`, `image-loading-lazy-in-cross-origin-iframe-002`, `position-relative-016`, `flex-item-compressible-002`, `transition-behavior`, `CSSAnimation-startTime`, `scroll-animations` ×2, `wasm/core/imports.wast.js`, `mixed-content/csp.https.window`, `FileAPI/url/sandboxed-iframe` |

Six WebAudio `PannerNode` files show a lower fraction only because they now
reach far more subtests, for example 4 of 5 → 5 of 108. The PannerNode work
lets the files run further, and they pass more subtests than before.

## Largest gaps (ranked by files lost)

| Area | Files | Score | Main failure mode |
|---|---|---|---|
| css | 8,291 | 52.2 % | 410 timeouts, 402 errors (mostly `css-typed-om`) |
| html | 9,211 | 58.4 % | 1,158 timeouts |
| svg | 800 | 19.9 % | 240 timeouts |
| referrer-policy | 1,482 | 57.0 % | 219 timeouts in the generated tests |
| editing | 734 | 15.7 % | 28,169 / 115,050 subtests — execCommand behaviour |
| websockets | 800 | 30.2 % | 135 harness errors |
| speculation-rules | 551 | 2.4 % | Now measured: 394 errors, 97 timeouts — not implemented |
| content-security-policy | 949 | 45.1 % | 213 timeouts |
| fetch | 948 | 47.8 % | 109 timeouts |
| navigation-api | 522 | 5.5 % | 175 timeouts, 58 errors — API largely missing |
| IndexedDB | 820 | 56.6 % | 28 timeouts, 21 errors |
| wasm | 538 | 38.2 % | 34,367 / 192,109 subtests |
| webaudio | 349 | 23.0 % | |
| WebCryptoAPI | 340 | 22.5 % | 62 errors |
| webrtc | 270 | 7.9 % | 134 timeouts |
| service-workers | 332 | 26.2 % | 117 timeouts |

Out of scope or device-bound, as yesterday: `webnn`, `ai`, `webmcp`, `webxr`,
`webcodecs`, `bluetooth`, `webtransport`, `fedcm`, `webauthn`, payment and
sensor APIs, and the accessibility-tree suites.

## Suggested next steps

1. **Module-script access violation.** This is six reproducible crashes and a
   possible memory-safety bug; bisect `024f6590..4f38d6c3` with
   `script-module-import-static.sub.html`.
2. **The reproducing regressions above.** Start with text modules and Event
   activation, which are each one cause across several files, then
   `meta-commented-out` and `abort-while-navigating`, which point at specific
   commits.
3. **Query-encoding frame `location`.** These are the five files left open
   by PR #1254.
4. **CSS Typed OM.** Still the largest block of harness errors in `css`.
5. **html/semantics + html/browsers timeouts, and `referrer-policy/gen`.**
   These share the `/common/security-features/` harness.
6. **Run with 24 jobs.** 48 batch browsers plus the server ran the machine
   out of memory, and the memory-starved half produced most of the noise
   above.

## Reproducing

```sh
cd ~/wpt && ./wpt serve --config wpt-config.json   # browser_host=localhost, ssl type=openssl
CURL_CA_BUNDLE=<openssl base_path>/cacert.pem \
  scripts/wpt-full.py --wpt-root=~/wpt --jobs=24 --results=full.jsonl
```

On Windows, put `C:/msys64/ucrt64/bin` on `PATH` for the runner: started from
a shell without it, every browser exits with `0xC0000135`, a DLL that was not
found. Pass individual tests to the runner with `--paths-from`, because
MSYS2 rewrites positional arguments that start with `/` into Windows paths.
