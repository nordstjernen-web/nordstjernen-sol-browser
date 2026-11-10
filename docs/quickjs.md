# QuickJS or quickjs-ng

The JavaScript binding runs on one of two QuickJS engines, both in the
tree, picked at configure time with the `quickjs` option:

```sh
meson setup builddir                          # default: Bellard's QuickJS
meson setup builddir -Dquickjs=quickjs        # the same, explicitly
meson setup builddir -Dquickjs=quickjs-ng     # the quickjs-ng fork
```

An existing build directory switches with
`meson configure builddir -Dquickjs=quickjs-ng`.

| | `quickjs` (default) | `quickjs-ng` |
|---|---|---|
| Source | `src/quickjs-bellard/`, an in-tree fork of [bellard/quickjs](https://github.com/bellard/quickjs) | `src/quickjs/`, an in-tree fork of [quickjs-ng](https://github.com/quickjs-ng/quickjs) |
| Version | master `e585640` (2026-11-10) plus the local changes below | quickjs-ng 0.18.0 (master `1b519fa`) plus the browser hooks; refresh history in [`vendored-engine-updates.md`](vendored-engine-updates.md) |
| About page | `QuickJS 2026-06-04` | `QuickJS 0.18.0` |
| Heap hardening | cage, caged values, compressed fields, pointer tables | the same |
| `Intl`, `Temporal` | upstream's own (`intl/`, `temporal/`) | the fork's ICU-free implementation |
| Engine scripts | embedded as source, compiled in each process | embedded as bytecode |
| CI | every workflow | none; build it locally (see below) |

Both builds carry the same Web API surface. The whole binding, `src/js.c`
and its satellites, is written against the quickjs-ng API; Bellard's
engine gets that API partly from the engine itself, which exports the
browser hooks listed below, and partly from a small adapter.

Why Bellard's engine became the default, and what changed when it did, is
in [`quickjs-bellard-default.md`](quickjs-bellard-default.md).

## The in-tree fork of Bellard's QuickJS

`src/quickjs-bellard/` holds upstream's library sources (`quickjs.c`,
`dtoa.c`, `libregexp.c`, `libunicode.c`, `cutils.c`, `intl/`, `temporal/`),
the host tools (`qjs.c`, `qjsc.c`, `quickjs-libc.c`, `run-test262.c`) and
the engine tests. `src/quickjs-bellard/meson.build` is loaded with
`subdir()`, compiles the library with upstream's defines (`_GNU_SOURCE`,
`CONFIG_VERSION`, `-fwrapv`, `CONFIG_TEMPORAL_INTL`) into a static library
with hidden symbols and exposes it as `libquickjs`. Native builds also get
`qjs-bellard`, `qjsc-bellard` and (not built by default)
`run-test262-bellard` in the build directory.

Local changes, by kind:

1. **Heap hardening** (`quickjs-cage.c`, `quickjs-cage.h`,
   `quickjs-cptr.h`, `quickjs-hardening.h`, `list.h` and the rewrites in
   `quickjs.c`): see below and [`heap-hardening.md`](heap-hardening.md).
2. **Browser hooks** at the end of `quickjs.c`, declared in `quickjs.h`
   under "Browser hooks".
3. **Security fixes** the pinned upstream commit still lacks (the table
   below). They were a patch series while the engine was fetched by a
   wrap and are applied to the sources now.

### Security fixes carried

Each fixes a fault reported upstream that the base commit still has, or
ports a fix the quickjs-ng fork carries for a fault Bellard's code shares.
Drop one when upstream fixes it.

| Fix | Fault | Reported |
|---|---|---|
| parser stops at the scope limit | Scope indexes are 16-bit operands with no limit, so a function with more than 65535 block scopes resolved variables against the wrong scope. The parser raises a SyntaxError. | bellard/quickjs#610, point 10 |
| zero-length copies | `memcpy`/`memcmp` with a NULL pointer and length 0 in typed-array `slice` after a species constructor shrinks the buffer and in empty `\q{}` alternatives. | #610, points 7 and 8 |
| `Math.sumPrecise` renormalises before overflow | A 56-bit limb passed 2^63 between renormalisations and gave a wrong sum for 300 values near 2^53. | #610, point 9 |
| async generator drains its queue | A completed async generator settled one queued request and left the rest pending forever. | #610, point 5 |
| number parser clamps the exponent | `js_atod` overflowed an `int`, so `"1000000000e2147483639"` parsed as 0. | fork `12bc082` |
| linear canonical ordering | `normalize()` on 200 000 combining marks took 27 s; long runs are counting-sorted. | fork `877dc65` |
| allocator refuses oversized requests | Size rounding wrapped for requests near `SIZE_MAX` and returned a tiny block. | fork `55b3317` |
| in-place concatenation skips atoms | Growing a string that is also an atom made property lookups by that atom miss. | fork `d766a7f` |
| OOM leaks and NULL dereferences | A leak in `BigInt` `**`, and NULL dereferences on allocation failure in bound functions, class brands and destructuring. | fork `8d4fd06` |
| BJSON resets the SAB mark | Refusing a SharedArrayBuffer left its cycle mark set. | fork `7b714f0` |
| BJSON reader holds object references | A duplicate key could free an object the back-reference table still named. | fork `15a00d4` |
| array loops poll interrupts | `includes` on `{length: 2**53 - 1}` could not be stopped. | fork `dc611ad` |

### Moving to a newer upstream commit

The engine is a fork now, so a refresh is a merge, as for the other
vendored engines ([`vendored-engine-updates.md`](vendored-engine-updates.md)):

1. Clone bellard/quickjs, check out the base commit named in the table
   above and the new one.
2. 3-way merge each file (`git merge-file ours base theirs`). Everything
   but `quickjs.c`, `quickjs.h`, `quickjs-libc.c` and `list.h` is
   unmodified and can be copied.
3. Declare any new pointer field of a heap structure `JS_CPTR(T)`, add it
   to the `BELLARD_FIELDS` table in `scripts/quickjs-cage-rewrite.py`, run
   `scripts/quickjs-cage-rewrite.py --apply --engine quickjs`, and convert
   the sites it reports by hand.
4. Compile `quickjs.c` once with `-Wall` and read every `-Wint-conversion`
   and `-Wincompatible-pointer-types` warning: a compressed field read raw,
   or a raw field read through `js_cload`, shows up there. The library
   target itself builds with `-w`.
5. Update the base commit in the table, then run the checks below.

## Browser hooks in Bellard's engine

Declared in `src/quickjs-bellard/quickjs.h`:

| Hook | Purpose |
|---|---|
| `JS_SetCanCompileStrings` | `eval()` and the `Function` constructors ask the browser first; a Content Security Policy without `'unsafe-eval'` refuses them with an `EvalError` |
| `JS_GetCallerRealm`, `JS_GetFunctionRealm`, `JS_GetPendingJobRealm` | the realm of the caller of the running native function, of a function, and of the next promise job |
| `JS_CloneCFunction` | a copy of a native function in another realm, for a frame's own copy of an interface |
| `JS_RepointArrayBuffer` | moves an imported `WebAssembly.Memory` buffer to the instance's memory |
| `JS_SetImmutablePrototype`, `JS_AtomIsArrayIndex` | exported as they are |
| `JS_IsDate`, `JS_IsRegExp`, `JS_IsMap`, `JS_IsSet`, `JS_IsProxy`, `JS_IsDataView`, `JS_IsArrayObject`, `JS_IsErrorObject`, `JS_GetBoxedPrimitiveKind` | exact class tests |
| `JS_SetHeapCage`, `JS_Cage*` | the heap cage and its shared heap |

## The adapter: `src/ns_quickjs.h`

Engine code includes `"ns_quickjs.h"`, never `<quickjs.h>`. With the
quickjs-ng fork it is a plain include. With Bellard's engine
(`NS_QUICKJS_ORIGINAL`, set by `src/quickjs-bellard/meson.build`) it
supplies what is left of the quickjs-ng API:

- **Different signatures.** `JS_IsArray`, `JS_IsError` and `JS_IsBigInt`
  take no context in quickjs-ng; `JS_NewArrayBuffer` takes a realloc-style
  callback and a maximum length; `JS_NewContext` is wrapped so that every
  context gets `Intl` and `Temporal`, and `JS_NewTypedArray` pads a short
  argument list with `undefined`. The promise-rejection tracker's
  `is_handled` argument is `ns_js_bool` and the interrupt handler's first
  argument is `ns_js_interrupt_arg`.
- **quickjs-ng additions rebuilt from public calls.** `JS_NewStringUTF16`,
  `JS_ThrowDOMException`, `JS_IsStrictEqual`, `JS_GetVersion`,
  `JS_ToNumber`, `JS_GetPropertyInt64`, `JS_FreezeObject`,
  `JS_NewForwarder`, and `JS_EvalThis2`, which pads the source with the
  script's line and column offset because `JS_EvalThis` takes no position.
- **Fork-only hooks that still fall back**, listed next.

A binding change that calls a quickjs-ng function Bellard's engine lacks
fails to compile in the default build. Add the function to the engine
when it needs engine internals, or to `src/ns_quickjs.c` when public calls
are enough, rather than an `#ifdef` at the call site.

## Known differences on Bellard's engine

These quickjs-ng fork features are not ported yet, so the default build
differs from `-Dquickjs=quickjs-ng` here:

- **Named access across frames.** `frame.contentWindow.someId` looks the id
  up in the calling document rather than the frame's: only the fork's
  receiver-aware property hook (`get_own_property_receiver`) can tell
  which window is being read. Named access within a page is unaffected.
- **Platform object state.** `JS_IsHostAccess` is always true and there are
  no engine-private names, so the state the engine keeps on platform
  objects such as `XMLHttpRequest` shows up as their own properties, and a
  page's write to an attribute is stored as given instead of going through
  the interface's setter.
- **`this` brands.** `JS_SetCFunctionBrand` is a no-op; interface methods
  check their receiver themselves.
- **Language behaviour.** `RegExp.$1`–`$9`, `Function.prototype.caller` and
  `Error.captureStackTrace` are missing, and `error.stack` has no leading
  `Name: message` line.
- **Engine helper source.** `JS_EVAL_FLAG_HIDE_SOURCE` is 0, so
  `Function.prototype.toString` of an engine-implemented member defined in
  an engine script shows its source instead of `[native code]`.
- **Engine script bytecode.** The engine's own scripts are embedded as
  source and compiled in each process, about 11 ms per renderer; the
  quickjs-ng build embeds bytecode.
- **`Intl` and `Temporal`** are upstream's implementations, which upstream
  still calls experimental, and `JS_SetDefaultLocale` is a no-op.
- **`ShadowRealm`** is absent.

## Heap cage and compressed pointers

Both engines keep every GC allocation inside a 4 GB reserved address range
(`quickjs-cage.c`) and, in 64-bit builds without AddressSanitizer, box
`JSValue` as an 8-byte word whose pointer half is a 32-bit offset into
that range (`JS_CAGE_BOXING` in `quickjs.h`). Pointer fields inside heap
structures are declared `JS_CPTR(T)` and read and written through
`js_cload(T, field)` and `js_cstore(field, ptr)`. Rules when touching
engine code:

- A compressed slot can hold cage memory only. The encoder aborts with a
  message when handed a pointer from outside the cage, so a structure
  stored in a `JS_CPTR` field, in the opaque of a built-in class, or in a
  `JSValue` must come from `js_malloc` or `js_cage_shared_malloc`, never
  from libc `malloc`, a static, or a trusted block.
- Native code pointers and host pointers never sit in heap objects: C
  functions go through the code pointer table, host opaques through the
  external pointer table, contexts through the context table. Add a
  new native-pointer field only behind one of those tables. In Bellard's
  engine `JS_SetOpaque` picks the encoding by class id: a cage offset for
  a built-in class, an external pointer table index for any other, which
  includes the `Intl` and `Temporal` classes.
- `JSRuntime`, `JSContext`, `class_array`, the atom table and the shape
  hash live outside the cage in trusted blocks; a new field that points
  from a cage object to one of them must be a table index, not a pointer.
- An ArrayBuffer's backing store must be cage memory: `JS_NewArrayBuffer`
  with a foreign buffer throws. Take the memory from `JS_CageMalloc`
  (`ns_js_cage_shared_malloc` in browser code).
- `scripts/quickjs-cage-rewrite.py` lists the compressed fields of both
  engines and re-applies or checks their accesses (`--apply`, `--check`,
  `--engine quickjs` or `--engine quickjs-ng`).

`docs/heap-hardening.md` has the design, the inventory of what is still
raw, and the measurements.

## Checking a change

Build both configurations with no warnings, GCC and Clang:

```sh
meson setup builddir && meson compile -C builddir
meson setup builddir-ng -Dquickjs=quickjs-ng && meson compile -C builddir-ng
./scripts/dev.sh smoke
NS_BIN=$PWD/builddir-ng/src/gtk/nordstjernen ./scripts/dev.sh smoke
```

After a change to Bellard's engine, run its tests through the host tool:

```sh
cd src/quickjs-bellard
Q=../../builddir/src/quickjs-bellard/qjs-bellard
for t in test_closure test_language test_loop test_bigint; do $Q tests/$t.js; done
$Q --std tests/test_builtin.js && $Q --std tests/test_std.js && $Q --std tests/test_worker.js
$Q --experimental tests/test_intl.js
```

and test262, which found faults live pages had not when the hardening
was written for the quickjs-ng fork:

```sh
ninja -C builddir src/quickjs-bellard/run-test262-bellard
```

Clone tc39/test262 at the commit upstream's `Makefile` names
(`TEST262_COMMIT`), apply `tests/test262.patch`, link it as `test262` next
to `test262.conf` and run `run-test262-bellard -t -m -c test262.conf -a`.
The expected result is the content of `test262_errors.txt`. A pointer
stored outside the cage shows up as an abort naming it, which is the
intended failure mode.

After a change to the quickjs-ng fork, run its in-tree test files through
`qjs` (`src/quickjs/tests/test_*.js`) and `scripts/test262-run.sh`.
