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
| Version | quickjs-ng 0.18.0 (master `1b519fa`, less the commits the refresh log lists as not taken) plus the browser hooks; refresh history in [`vendored-engine-updates.md`](vendored-engine-updates.md) | master `e5ac135` (2026-11-09), pinned by commit, plus twelve local security patches |
| About page | `QuickJS 0.18.0` | `QuickJS 2026-06-04` |
| CI | every workflow | none; build it locally (see below) |

Both builds carry the same Web API surface. The whole binding — `src/js.c`
and its satellites — is written against the quickjs-ng API; the original
engine gets that API through a small adapter instead of a second binding.

## How the original engine is built

`subprojects/quickjs.wrap` pins a commit on the upstream `master` branch,
currently `e5ac135` (2026-11-09), the newest at the time of writing.
Upstream has not changed `VERSION` since the 2026-06-04 release, so the
About page still reads `QuickJS 2026-06-04`. Upstream ships only a
Makefile, so the wrap's `patch_directory` overlays
`subprojects/packagefiles/quickjs/meson.build`, which compiles the library
objects the Makefile builds (`quickjs.c`, `dtoa.c`, `libregexp.c`,
`libunicode.c`, `cutils.c`) with the same defines (`_GNU_SOURCE`,
`CONFIG_VERSION`, `-fwrapv`) into a static library with hidden symbols.
Master's experimental Temporal and Intl (`CONFIG_TEMPORAL_INTL`, the
`intl/` and `temporal/` directories) are not compiled, and neither is
`quickjs-libc.c`; the browser never calls `JS_AddIntrinsicTemporal` or
`JS_AddIntrinsicIntl`. Nothing is installed. A build with
`--wrap-mode=nodownload` needs the checkout already in
`subprojects/quickjs/`.

### Local patches

The wrap's `diff_files` applies the patches in
`subprojects/packagefiles/quickjs-patches/`, in order, on every fresh
fetch. Each fixes a fault reported upstream that the pinned commit still
has, in code this build compiles, or ports a fix the quickjs-ng fork in
`src/quickjs/` carries for a fault Bellard's code shares. Each is written
to be dropped once upstream fixes it:

| Patch | Fault | Reported |
|---|---|---|
| `0001-parser-stop-at-scope-limit` | Scope indexes are 16-bit operands with no limit, so a function with more than 65535 block scopes resolved variables against the wrong scope (an initialised `let` read as uninitialised). The parser now raises a SyntaxError and stops at the next token. | bellard/quickjs#610, point 10 |
| `0002-zero-length-copies` | `memcpy`/`memcmp` with a NULL pointer and length 0, undefined behaviour, in typed-array `slice` after a species constructor shrinks the buffer and in empty `\q{}` alternatives of `v`-flag classes. | #610, points 7 and 8 |
| `0003-sum-precise-renormalize-before-overflow` | `Math.sumPrecise` let a 56-bit limb pass 2^63 between renormalisations and returned a wrong sum for 300 values near 2^53. | #610, point 9 |
| `0004-async-generator-drain-queue` | A completed async generator settled one queued request and left the rest pending forever, and `next()` calls queued behind `return()` never settled. | #610, point 5 |
| `0005-number-parser-clamp-exponent` | `js_atod` overflowed an `int` adjusting a huge decimal exponent, so `"1000000000e2147483639"` parsed as 0. | fork `12bc082` |
| `0006-normalize-linear-canonical-ordering` | Canonical ordering of combining marks was an insertion sort, so `normalize()` on 200 000 marks took 27 s. Long runs are now counting-sorted. | fork `877dc65` |
| `0007-allocator-refuse-oversized-requests` | The block allocator's size rounding wrapped for requests near `SIZE_MAX` and returned a tiny block. | fork `55b3317` |
| `0008-concat-in-place-skip-atoms` | In-place concatenation could grow a string that is also an atom, so property lookups by that atom missed. | fork `d766a7f` |
| `0009-oom-leaks-and-null-derefs` | A leak in `BigInt` `**` on a thrown RangeError, and NULL dereferences or unchecked buffers on allocation failure in bound functions, class brands and destructuring. | fork `8d4fd06` |
| `0010-bjson-reset-sab-mark` | Refusing a SharedArrayBuffer left its cycle mark set, so the next write reported a circular reference. | fork `7b714f0` |
| `0011-bjson-reader-holds-object-refs` | The BJSON reader's back-reference table held no references, so a duplicate key could free an object the table still named. | fork `15a00d4` |
| `0012-array-loops-poll-interrupts` | The generic `Array.prototype` loops never polled the interrupt handler, so `includes` on `{length: 2**53 - 1}` could not be stopped. | fork `dc611ad` |

The rest of #610 is outside this build: points 1 to 4, 6 and 11 are in
Temporal, Intl and `quickjs-libc.c`, point 12 is not reachable from
script, and upstream treats bytecode as trusted. Points 1 and 2, the two
high-severity Temporal reports, are fixed upstream at `f2e4461`. The
quickjs-ng fork shared points 5 and 10 and carries the same fixes; it
already had points 7 and 8. Of the fork's other security fixes, eight
did not apply: Bellard's code already had the fix, or lacks the feature
(string slices, the regexp and symbol serializer tags, `prepareStackTrace`).

### Moving to a newer upstream commit

```sh
scripts/bump-bellard-quickjs.sh            # newest master
scripts/bump-bellard-quickjs.sh <commit>   # a given commit
```

The script rewrites `revision` in the wrap, deletes `subprojects/quickjs/`
and fetches again, which applies the patches. It fails when a patch no
longer applies: that is usually upstream having fixed the same fault, so
check the upstream commit, drop the patch from `diff_files` and the table
above, and rerun. Then reconfigure, drop any adapter shim for a call
upstream now exports (as `JS_ToObject` was dropped at `84e723f`), and rerun
the checks below.

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

## Heap cage and compressed pointers

The fork keeps every GC allocation inside a 4 GB reserved address range
(`quickjs-cage.c`) and, in 64-bit builds without AddressSanitizer,
boxes `JSValue` as an 8-byte word whose pointer half is a 32-bit offset
into that range (`JS_CAGE_BOXING` in `quickjs.h`). Pointer fields inside
heap structures are declared `JS_CPTR(T)` and read and written through
`js_cload(T, field)` and `js_cstore(field, ptr)`. Rules when touching
engine code:

- A compressed slot can hold cage memory only. The encoder aborts with a
  message when handed a pointer from outside the cage, so a structure
  stored in a `JS_CPTR` field, in `u.opaque` through
  `JS_SetOpaqueInternal`, or in a `JSValue` must come from `js_malloc`
  or `js_cage_shared_malloc`, never from libc `malloc`, a static, or a
  trusted block. The Intl classes, which allocate with libc, keep their
  structure in the external pointer table instead; follow that pattern
  for anything else that cannot live in the cage.
- Native code pointers and host pointers never sit in heap objects: C
  functions go through the code pointer table, host opaques through the
  external pointer table, contexts through the context table. Add a
  new native-pointer field only behind one of those tables.
- `JSRuntime`, `JSContext` and `class_array` live outside the cage in
  trusted blocks; a new field that points from a cage object to one of
  them must be a context-table index, not a pointer.
- The machinery lives in `quickjs-cptr.h` (compressed fields) and
  `quickjs-hardening.h` (cage masking and the tables), each included
  from `quickjs.c` at one line, and `scripts/quickjs-cage-rewrite.py`
  lists the compressed fields and re-applies or checks their accesses
  (`--apply`, `--check`).
- The `-Dquickjs=quickjs` configuration has none of this; the adapter
  supplies malloc-backed `JS_Cage*` shims and the browser runs on the
  process heap there. It also has no `Intl`: the original engine's Intl
  and Temporal are not built (see above), so pages that need them differ.

`docs/heap-hardening.md` has the design, the inventory of what is still
raw, and the measurements.

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

After an engine change, run the in-tree test files through the host
tool and the headless pages the hardening work uses (`qjs
src/quickjs/tests/test_*.js`, then a settled live-site capture with
`--timing --settle-ms=2000 --time-ms=1000 --dump=png:…`): a pointer
stored outside the cage shows up as an abort naming it, which is the
intended failure mode.
