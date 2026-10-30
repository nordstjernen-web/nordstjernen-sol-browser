# QuickJS or quickjs-ng

The JavaScript binding runs on one of two QuickJS engines, picked at configure
time with the `quickjs` option:

```sh
meson setup builddir                          # default: in-tree quickjs-ng fork
meson setup builddir -Dquickjs=quickjs-ng     # the same, explicitly
meson setup builddir -Dquickjs=quickjs        # Fabrice Bellard's original QuickJS
```

An existing build directory switches with
`meson configure builddir -Dquickjs=quickjs`.

| | `quickjs-ng` (default) | `quickjs` |
|---|---|---|
| Source | `src/quickjs/`, an in-tree fork of [quickjs-ng](https://github.com/quickjs-ng/quickjs) | [bellard/quickjs](https://github.com/bellard/quickjs), the original engine |
| How it gets into the build | always in the tree | fetched at configure time by `subprojects/quickjs.wrap`, never vendored |
| Version | quickjs-ng 0.17.0 (master `7b5f588`, less the commits the refresh log lists as not taken) plus the browser hooks; refresh history in [`vendored-engine-updates.md`](vendored-engine-updates.md) | master `19d6bd8` (2026-10-26), pinned by commit, plus one sort patch |
| About page | `QuickJS 0.17.0` | `QuickJS 2026-06-04` |
| CI | every workflow | none; build it locally (see below) |

Both builds carry the same Web API surface. The whole binding — `src/js.c`
and its satellites — is written against the quickjs-ng API; the original
engine gets that API through a small adapter instead of a second binding.

## How the original engine is built

`subprojects/quickjs.wrap` pins a commit on the upstream `master` branch,
currently `19d6bd8` (2026-10-26), eighty-one commits past the 2026-06-04 release.
Upstream has not changed `VERSION` since, so the About page still reads
`QuickJS 2026-06-04`. Upstream ships only a Makefile, so the wrap's
`patch_directory` overlays `subprojects/packagefiles/quickjs/meson.build`,
which compiles the library objects the Makefile builds (`quickjs.c`,
`dtoa.c`, `libregexp.c`, `libunicode.c`, `cutils.c`) with the same defines
(`_GNU_SOURCE`, `CONFIG_VERSION`, `-fwrapv`) into a static library with
hidden symbols. Master's experimental Temporal and Intl
(`CONFIG_TEMPORAL_INTL`, the `intl/` and `temporal/` directories) are not
compiled; the browser never calls `JS_AddIntrinsicTemporal` or
`JS_AddIntrinsicIntl`. Nothing is installed. A build with
`--wrap-mode=nodownload` needs the checkout already in
`subprojects/quickjs/`.

One source patch rides on top, named by `diff_files`:
`quickjs-sort-calls-comparator.patch`. The original `Array.prototype.sort`
skips the comparator when both values are the same, which the fork, V8,
SpiderMonkey and JavaScriptCore never do. jQuery 4's `uniqueSort` counts on
that call to spot duplicates, so on the unpatched engine `$(a).add(a)` and
`.closest()` return the same element twice.

To move forward, point `revision` in the wrap at a newer `master` commit,
delete `subprojects/quickjs/`, reconfigure, regenerate the patch if it no
longer applies, drop any adapter shim for a call upstream now exports (as
`JS_ToObject` was dropped at `84e723f`), and rerun the checks below.

## The adapter: `src/ns_quickjs.h`

Engine code includes `"ns_quickjs.h"`, never `<quickjs.h>`. With the fork it
is a plain include. With the original engine (`NS_QUICKJS_ORIGINAL`, set by
`meson.build`) it supplies the quickjs-ng API the binding uses:

- **Different signatures.** `JS_IsArray`, `JS_IsError` and `JS_IsBigInt`
  take no context in quickjs-ng; `JS_NewArrayBuffer` takes a realloc-style
  callback and a maximum length; `JS_NewContext` is wrapped so the adapter
  can learn the engine's class IDs, and `JS_NewTypedArray` pads a short
  argument list with `undefined`, because the original typed-array
  constructor reads three arguments whatever `argc` says. These are macros
  over functions in `src/ns_quickjs.c`. The promise-rejection tracker's `is_handled` argument
  is `ns_js_bool`, which is `bool` on quickjs-ng and `JS_BOOL` on the
  original, and the interrupt handler's first argument is
  `ns_js_interrupt_arg`, a `JSContext` on quickjs-ng and a `JSRuntime` on
  the original.
- **quickjs-ng additions.** `JS_IsArrayBuffer`, `JS_IsDataView` and
  `JS_GetTypedArrayType` compare class IDs learned once, from objects made
  in the first context. `JS_ToObject`, `JS_NewStringUTF16`,
  `JS_ThrowDOMException`, `JS_IsStrictEqual`, `JS_AtomIsArrayIndex` and
  `JS_GetVersion` are rebuilt from public calls. `JS_EvalThis2` pads the
  source with the script's line and column offset, because the original
  `JS_EvalThis` takes no position, so errors still point at the right line
  of the HTML document.
- **Fork-only hooks.** The fork's own additions have no equivalent and fall
  back, as described below.

The bytecode cache lives in memory in each process, so the two engines'
bytecode, which is not interchangeable, never meets.

## Known differences on the original engine

The fork carries browser hooks and compatibility changes that the original
engine lacks. On `-Dquickjs=quickjs` these differ from the default build:

- **Named access across frames.** `frame.contentWindow.someId` looks the id
  up in the calling document rather than the frame's. Frames share their
  parent's `Window.prototype` chain, and only the fork's receiver-aware
  property hook can tell which window is being read. Named access within a
  page, `window.someId` or a bare `someId`, is unaffected.
- **Calling realm.** `JS_GetCallerRealm` reports the realm of the native
  function being called rather than the realm of its caller, and
  `JS_GetFunctionRealm` reports the current realm rather than the
  function's own. Across frames, this can change which document a
  cross-realm `createElement` or custom element is tagged with, and puts
  `window.event` on the top-level window while a listener from a frame
  runs.
- **Native functions in frames.** A frame gets its own copies of the
  page's interfaces. The fork clones each native function into the frame's
  realm; the original engine cannot, so the frame's copy forwards to the
  page's function and the native code runs in the page's realm.
- **Platform object state.** `JS_IsHostAccess` is always true, so the state
  the engine keeps on platform objects such as `XMLHttpRequest` and
  `AbortController` shows up as their own properties, and a page's write to
  an attribute (`xhr.timeout = '1500'`) is stored as given instead of going
  through the interface's setter.
- **CSP and `eval`.** The fork asks the browser before `eval()` or a
  `Function` constructor compiles a string, so a Content Security Policy
  without `'unsafe-eval'` refuses them with an `EvalError`. The original
  engine has no such hook and runs them. String timers and WebAssembly
  compilation are checked by the browser itself and are refused on both
  engines.
- **WebAssembly memory.** A view made on an imported
  `WebAssembly.Memory`'s `buffer` before the module is instantiated is
  detached by instantiation instead of being repointed at the instance's
  memory. Re-reading `memory.buffer` gives a working buffer, as it does
  after `grow()` on both engines.
- **Language behaviour.** The fork's compatibility changes are absent:
  `RegExp.$1`–`$9`, `Function.prototype.caller`, `Error.captureStackTrace`
  and `using` declarations are missing, and `error.stack`
  has no leading `Name: message` line.
- **Promise jobs in frames.** The original engine does not expose its job
  queue, so `JS_GetPendingJobRealm` is NULL and a promise reaction from a
  frame runs against whichever document is current when the queue drains,
  instead of switching to the frame whose code it continues.
- **Engine helper source.** Scripts the engine evaluates for itself are not
  hidden (`JS_EVAL_FLAG_HIDE_SOURCE` is 0), so `Function.prototype.toString`
  of an engine-implemented member defined in such a script shows its source
  instead of `[native code]`.
- **Speed.** `JS_AtomIsArrayIndex` goes through a string, so indexed access
  on host objects (collections, `frames[i]`) is slower.

## Checking a change

Build both configurations with no warnings, GCC and Clang:

```sh
meson setup builddir && meson compile -C builddir
meson setup builddir-quickjs -Dquickjs=quickjs && meson compile -C builddir-quickjs
NS_BIN=$PWD/builddir-quickjs/src/gtk/nordstjernen ./scripts/dev.sh smoke
```

A binding change that calls a quickjs-ng function the original engine lacks
fails to compile in `builddir-quickjs`; add the function to
`src/ns_quickjs.c` rather than an `#ifdef` at the call site.
