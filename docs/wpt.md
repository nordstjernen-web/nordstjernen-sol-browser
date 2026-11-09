# Testing Nordstjernen with the web-platform-tests (WPT)

Nordstjernen can run [web-platform-tests](https://github.com/web-platform-tests/wpt)
testharness.js tests headlessly and report per-subtest results. This is
the cross-browser conformance suite shared by Chromium, Gecko, and
WebKit; running slices of it against Nordstjernen is a fast way to find
and track engine gaps.

## Scope

Supported: **testharness.js tests** — plain `.html` tests and the
server-generated `.any.html` / `.window.html` wrappers for `.any.js`
and `.window.js` files.

Reftests run through a separate runner, `scripts/wpt-reftest.py`; see
[Reftests](#reftests) below.

Not supported by `wpt-run.sh`: reftests, crashtests, `.worker.js` /
service-worker tests, wdspec tests, manual tests, and print tests.
During enumeration
the runner only picks up `.html`/`.htm`, `.any.js`, and `.window.js`
files that reference `testharness.js`, and skips `resources/`,
`support/`, `tools/`, and `crashtests/` directories plus `*-ref.html`
and `*-manual.html` files. A test that declares
`<meta name="variant">` entries is run once per variant.

## One-time setup

Clone WPT and install its hosts entries (the WPT server expects
`web-platform.test` and friends to resolve to 127.0.0.1):

```sh
git clone --depth 1 https://github.com/web-platform-tests/wpt.git ~/wpt
cd ~/wpt && ./wpt make-hosts-file | sudo tee -a /etc/hosts
```

WPT's server only needs Python 3; everything else is vendored in its
checkout.

## Running tests

Build the browser first (`scripts/dev.sh build`), then:

```sh
# A directory of tests
scripts/wpt-run.sh --wpt-root=~/wpt dom/events

# Individual tests (paths relative to the WPT checkout)
scripts/wpt-run.sh --wpt-root=~/wpt url/url-tojson.any.js dom/nodes/Element-tagName.html

# Just enumerate, don't run
scripts/wpt-run.sh --wpt-root=~/wpt --list dom/events
```

The runner starts `./wpt serve` itself (and stops it on exit) unless a
server is already reachable; pass `--no-serve` to require an external
server. Other options: `--base=URL` (default
`http://web-platform.test:8000`; with a `localhost`/`127.0.0.1` base the
auto-started server is configured for `localhost`, which suits
origin-insensitive tests without hosts entries), `--timeout-ms=N` per
test (default 15000), and `--results=FILE` to append one JSON line per
test:

```json
{"test":"/url/url-tojson.any.html","exit":0,"result":{"harness":"OK","message":null,"subtests":[...]}}
```

The runner exits 0 only when every test file ran without failures.
`NS_BIN` overrides the browser binary (default
`builddir/src/gtk/nordstjernen`) and `NS_WPT_ROOT` supplies a default
for `--wpt-root`.

## Reftests

Most CSS layout tests in WPT are reftests: a test page and a reference
page that must look the same (`<link rel="match">`) or different
(`<link rel="mismatch">`). `scripts/wpt-reftest.py` renders both with
`--dump=png` at 800x600 and compares them the way wptrunner does:
pixel for pixel, or within the ranges of a `<meta name="fuzzy">`. A
test passes when it matches at least one `match` reference and differs
from every `mismatch` reference. It needs Python 3 with Pillow.

```sh
scripts/wpt-reftest.py --wpt-root=~/wpt css/CSS2/floats css/css-flexbox
scripts/wpt-reftest.py --wpt-root=~/wpt --results=before.tsv css/css-grid
scripts/wpt-reftest.py --wpt-root=~/wpt --baseline=before.tsv css/css-grid
```

`--results` writes one tab-separated line per test (path, status,
largest channel difference, differing pixels, reference, note) as the
tests finish, and `--resume` continues a run that stopped. With
`--baseline` the run lists the tests that changed against an earlier
results file and exits 1 when a test that passed there fails now, so
it can gate a change. `--diffs=DIR` saves a test | reference |
difference strip for every failure. `--chrome=PATH` renders with
Chrome instead, which tells the tests Nordstjernen fails apart from
the ones Chrome fails too.

The pages are served by a static HTTP server rooted at the checkout,
so `.sub.` tests, print tests and paged reftests are skipped, and the
references of a reference are not followed. Nordstjernen's PNG is as
tall as the document; both images are padded to 800x600 with the
page's own background colour before they are compared.

## Related scripts

- `scripts/wpt-score.sh --wpt-root=DIR [PATH...]` — runs the tracked
  slice (or part of it) through `wpt-run.sh` and updates
  [wpt-scores.md](wpt-scores.md), `wpt-subtests.tsv`, and a snapshot in
  `wpt-runs/`.
- `scripts/wpt-fast.sh [--fast-root=DIR] [--jobs=N] [PATH...]` — runs
  the [wpt-fast](https://github.com/nordstjernen-web/wpt-fast) checkout
  in parallel; scores land in [wpt-fast-scores.md](wpt-fast-scores.md).
- `scripts/wpt-local.sh PATH...` — quick iteration against a plain
  static HTTP server instead of `./wpt serve` (no multi-origin or
  server-side tests).
- `scripts/wpt-estimate.sh --wpt-root=DIR [--sample=N]` — extrapolates
  a whole-suite pass count from a random sample; needs a running WPT
  server.

## Running a single test by hand

The runner is a thin loop around the browser's headless WPT mode,
which is also usable directly against any URL serving a testharness.js
test:

```sh
./builddir/src/gtk/nordstjernen --wpt http://web-platform.test:8000/dom/events/CustomEvent.html
./builddir/src/gtk/nordstjernen --wpt --wpt-timeout-ms=30000 http://localhost:8000/some/test.html
```

Output on stdout, one line per subtest:

```
WPT HARNESS OK
WPT PASS sync pass
WPT FAIL sync fail | assert_equals: math expected 3 but got 2
WPT SUMMARY total=2 pass=1 fail=1 timeout=0 notrun=0 precondition_failed=0
WPT JSON {"harness":"OK","message":null,"subtests":[...]}
```

Exit codes: `0` — harness OK (or `PRECONDITION_FAILED`) and every
subtest passed; `1` — at least one subtest failed, timed out, or did
not run (or the harness errored); `2` — the page never produced results
before the timeout (testharness.js missing, or tests hung).

## How it works

`--wpt` makes the headless driver inject a hook script
(`data/js/wpt-hook.js`, embedded into the binary at build time) into
the fresh JS context before any document script runs. The hook places
an accessor trap on `globalThis.add_completion_callback`; when
testharness.js defines that function the trap restores the plain
function and registers a completion callback with the harness —
immediately if possible, otherwise from a microtask, since
testharness.js creates its internal `Tests` object after exposing the
API. The driver's poll also registers the callback as a last resort if
neither attempt succeeded. On completion the callback serializes the
harness status and every subtest result into globals that the driver
polls from C (`src/headless.c`), prints, and turns into the exit code.
Because registration happens before the test script executes, even
tests that complete synchronously during parsing are captured. No
modification of the WPT checkout (e.g. replacing
`testharnessreport.js`) is needed.
