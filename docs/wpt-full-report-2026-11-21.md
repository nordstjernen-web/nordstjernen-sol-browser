# Full WPT run — 2026-11-21

Every testharness.js URL in the WPT `MANIFEST.json` (40,472 URLs, all
variants: `.any`/`.window`/`.worker`/`.sharedworker`/`.serviceworker`,
`?variant`, `.https`, `.h2`), run through the headless `--wpt-batch` driver
with `scripts/wpt-full.py`.

| | |
|---|---|
| Browser | `main` @ `024f6590`, Windows 11 / MSYS2 UCRT64 build |
| WPT checkout | `web-platform-tests/wpt` @ `9544d3086` (2026-11-19) |
| Server | `./wpt serve`, `browser_host: localhost`, OpenSSL-generated certs for `localhost` |
| Timeouts | 10 s per test, 30 s for `timeout=long` |
| Parallelism | 10 → 24 → 48 batch browsers (6 cores / 12 threads; CPU-idle, I/O- and timeout-bound) |
| Wall time | ~55 min (sum of per-test time: 30.2 h) |
| Raw results | `builddir/wpt-full-2026-11-21.jsonl` (one line per URL, with every subtest status) |

## Headline

| Metric | Value |
|---|---|
| Test files | 40,472 |
| Files fully passing (harness OK, every subtest PASS) | **11,907 (29.4 %)** |
| File-weighted score (mean per-file pass fraction) | **37.2 %** — **41.0 %** over the 36,741 files actually measured |
| Subtests passing | 621,546 / 2,058,128 (30.2 %) |
| Subtests passing, excluding `encoding/` | 519,609 / 962,575 (**54.0 %**) |

`encoding/` contributes 1.1 M of the 2.06 M subtests (one per code point in the
legacy-CJK tables), so the raw subtest ratio is dominated by it; the
file-weighted score is the better overall figure.

Harness outcome per file:

| Harness | Files |
|---|---|
| OK | 30,151 |
| TIMEOUT | 4,994 |
| NORESULT | 3,804 (3,731 of them the TLS issue below) |
| ERROR | 1,516 |
| CRASH | 7 |

## Caveat: 3,731 HTTPS files were not measured

For static `.https.html` files wptserve answers without `Content-Length`
and closes the TLS connection without `close_notify`. libcurl on OpenSSL 3
reports `SSL_read: unexpected eof while reading` (curl's own CLI fails the
same way, exit 56), the browser discards the document, and the test ends in
<1 s with no result. Generated wrappers (`.any.html` etc.) carry a
`Content-Length` and work. Affected: `speculation-rules` 491,
`mixed-content` 380, `html` 338, `service-workers` 271,
`upgrade-insecure-requests` 193, `content-security-policy` 190, `webxr` 170,
`webrtc` 110, `encrypted-media` 95, `client-hints` 83, ….

Chromium and Firefox accept a close-delimited HTTP/1.1 body that ends in a
bare TCP close, so this is also a real-world robustness gap, not only a
harness artefact. Treating an unexpected EOF on a close-delimited response as
end of body (OpenSSL's `SSL_OP_IGNORE_UNEXPECTED_EOF` semantics, applied only
when there is no `Content-Length`/chunked framing) would make these 9 % of the
suite measurable; rerun them afterwards with the same `--results` file after
deleting their rows.

## Strongest areas (file-weighted)

| Area | Files | Score | Subtests |
|---|---|---|---|
| url | 79 | 94.8 % | 12,207 / 12,443 |
| domxpath | 35 | 89.0 % | 1,144 / 1,171 |
| dom | 811 | 77.0 % | 63,191 / 66,573 |
| webstorage | 54 | 75.9 % | 1,270 / 1,291 |
| webmessaging | 167 | 69.8 % | 213 / 307 |
| FileAPI | 113 | 68.3 % | 1,311 / 1,594 |
| workers | 338 | 67.6 % | 901 / 1,539 |
| streams | 307 | 65.6 % | 3,867 / 4,368 |
| xhr | 442 | 62.1 % | 1,644 / 2,329 |
| resize-observer | 37 | 62.6 % | 67 / 123 |
| cors | 27 | 60.4 % | 241 / 463 |
| webgl | 25 | 59.2 % | 4,229 / 4,288 |

## Largest gaps (ranked by files lost = files − file-weighted passes)

In-scope areas only; AI-style APIs that the project deliberately does not
implement (`webnn`, `ai`, `webmcp`) and hardware/device APIs are listed
separately below.

| Area | Files | Score | Main failure mode |
|---|---|---|---|
| html | 9,211 | 52.7 % | 1,054 timeouts (`html/semantics` 413, `html/browsers` 364), 338 TLS |
| css | 8,291 | 52.1 % | 418 timeouts (`css/selectors` 193, view-transitions 56); 392 errors, 278 of them `css-typed-om` |
| encoding | 1,343 | 12.6 % | All 547 `*-form-*` legacy-CJK tests time out (each ~40 s) |
| referrer-policy | 1,482 | 41.8 % | 364 timeouts, 330 in `referrer-policy/gen` |
| svg | 800 | 19.9 % | 241 timeouts (`svg/painting`, `svg/geometry`) |
| content-security-policy | 949 | 33.4 % | 216 timeouts + 190 TLS |
| editing | 734 | 15.4 % | 28,135 / 114,938 subtests — execCommand behaviour |
| fetch | 948 | 35.5 % | 211 timeouts, 106 in `fetch/api` |
| websockets | 800 | 30.2 % | 135 harness errors |
| navigation-api | 522 | 5.3 % | 165 timeouts, 58 errors — API largely missing |
| IndexedDB | 820 | 43.5 % | 206 timeouts; the only **crashes** in the run |
| wasm | 538 | 33.4 % | 34,125 / 191,849 subtests |
| service-workers | 332 | 6.9 % | Not implemented: every `.serviceworker.html` variant (681 / 688) times out |
| webaudio | 349 | 22.2 % | |
| WebCryptoAPI | 340 | 21.4 % | 45 errors in `generateKey` |
| shadow-dom | 318 | 38.1 % | 9,329 / 9,971 subtests pass, but many files miss one or two |
| custom-elements | 212 | 17.5 % | |
| mathml | 218 | 17.8 % | |
| trusted-types | 247 | 26.0 % | 92 timeouts |
| selection | 172 | 34.3 % | |

Out of scope or device-bound, near 0 % as expected: `webnn` (610 files),
`ai` (195), `webmcp` (80), `webxr`, `bluetooth` (151 errors, needs
testdriver BiDi), `webcodecs`, `webtransport`, `fs`, `fedcm`, `webauthn`,
`encrypted-media`, `payment-*`, sensors, `webusb`/`serial`/`webhid`.
Accessibility-tree tests (`html-aam`, `accname`, `wai-aria`, `svg-aam`) load
but pass nothing: they need testdriver's accessibility hooks.

## Crashes

All seven are IndexedDB interleaved cursors, in every scope:

- `/IndexedDB/interleaved-cursors-large.any.{html,worker,sharedworker,serviceworker}.html`
- `/IndexedDB/interleaved-cursors-small.any.{worker,sharedworker,serviceworker}.html`

These are worth looking at first: a renderer crash may be a memory-safety bug.

## Suggested next steps (by return per effort)

1. **IndexedDB interleaved-cursor crash** — 7 files, possible security bug.
2. **TLS unexpected-EOF tolerance** for close-delimited bodies — makes 3,731
   files measurable and fixes real sites that close without `close_notify`.
3. **Legacy-CJK form-submission encoding** (`encoding/*-form-*`, 547 files,
   ~1 M subtests) — the tests submit a form into an iframe and read the URL
   back; all time out, so one fix to form submission with a legacy
   `accept-charset`/document encoding likely flips the whole group.
4. **CSS Typed OM** — 278 harness errors in `css/css-typed-om`, most likely a
   missing interface (`CSSStyleValue`/`attributeStyleMap`) breaking the
   whole file rather than individual subtests.
5. **html/semantics + html/browsers timeouts** (777 files) and
   **referrer-policy/gen** (330 timeouts): the generated tests share one
   harness (`/common/security-features/`), so one root cause likely blocks
   hundreds of files.
6. **Navigation API** (522 files at 5 %) and **service workers** (1,000+ files
   including the `.serviceworker` variants): large features, large payoff.

## Reproducing

```sh
cd ~/wpt && ./wpt serve --config wpt-config.json   # browser_host=localhost, ssl type=openssl
CURL_CA_BUNDLE=<openssl base_path>/cacert.pem \
  scripts/wpt-full.py --wpt-root=~/wpt --jobs=48 --results=full.jsonl
scripts/wpt-full.py --summary-only --wpt-root=~/wpt --results=full.jsonl --tsv=full.tsv
```

`--jobs` well above the core count is safe here: the run is dominated by
tests waiting for their timeout, and 48 jobs still left the 6-core machine
under-used. The `pregenerated` WPT certificate only covers
`web-platform.test`, so without hosts entries the `openssl` cert type is
needed for the `.https` tests to load at all.
