# QuickJS

The JavaScript binding runs on one engine: `src/quickjs-bellard/`, an
in-tree fork of Fabrice Bellard's [QuickJS](https://github.com/bellard/quickjs).

| | |
|---|---|
| Version | master `e585640` (2026-11-10) plus the local changes below |
| About page | `QuickJS 2026-06-04` |
| Heap hardening | cage, caged values, compressed fields, pointer tables |
| `Intl`, `Temporal` | upstream's own (`intl/`, `temporal/`) |
| Engine scripts | embedded as bytecode (`quickjs-bellard-bcgen`) |

The whole binding, `src/js.c` and its satellites, is written against the
quickjs-ng-style API; the engine supplies that API partly itself, through
the browser hooks listed below, and partly through a small adapter.

Bellard's engine has been the default since 1.0.47. Before that it was
fetched by `subprojects/quickjs.wrap` with a patch series; the heap
hardening rewrites several hundred field accesses in `quickjs.c`, which a
patch series cannot carry, so the engine became an in-tree fork. Until
2026-11 a second engine, a fork of quickjs-ng in `src/quickjs/`, could be
built with `-Dquickjs=quickjs-ng`; it and the option were removed.

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
ports a fix the former quickjs-ng fork carried for a fault Bellard's code
shares.
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

The engine is a fork, so a refresh is a merge against upstream `master`:

1. Clone bellard/quickjs, check out the base commit named in the table
   above and the new one.
2. Classify every carried file. One that only upstream changed is copied;
   one that only we changed is kept; one that both changed is 3-way merged
   (`git merge-file ours base theirs`), reviewing every hunk. Sources are
   listed explicitly in `src/quickjs-bellard/meson.build`, so a new
   upstream file in an unused area is simply not compiled.
3. Declare any new pointer field of a heap structure `JS_CPTR(T)`, add it
   to the `FIELDS` table in `scripts/quickjs-cage-rewrite.py`, run
   `scripts/quickjs-cage-rewrite.py --apply`, and convert
   the sites it reports by hand. A new native-pointer or `JSContext *`
   field goes through the matching pointer table instead.
4. Compile `quickjs.c` once with `-Wall` and read every `-Wint-conversion`
   and `-Wincompatible-pointer-types` warning: a compressed field read raw,
   or a raw field read through `js_cload`, shows up there. The library
   target itself builds with `-w`.
5. Fix the adapter if the binding no longer compiles, update the base
   commit in the table and add a row to the refresh log, then run the
   checks below.

The encoder aborts on a pointer from outside the cage and the lists abort
on a corrupted link, so a missed conversion fails loudly in the first test
run rather than silently.

Fork-local pieces a refresh touches, by file:

| File | What is ours |
|---|---|
| `quickjs-cage.c`, `quickjs-cage.h` | the cage allocator; no upstream counterpart |
| `quickjs-cptr.h`, `quickjs-hardening.h` | compressed fields, masking, pointer and context tables; included from `quickjs.c` |
| `list.h` | neighbour checks in `__list_add` and `list_del` |
| `quickjs.h` | `JS_CAGE_BOXING` layout, `JS_SetHeapCage`, `JS_Cage*`, and the browser hooks |
| `quickjs.c` | the cage and pointer-table block near the top of the file, the trusted block path in the arena front end, the `js_cload` / `js_cstore` rewrites the script reproduces, buffer pointers read through `js_abuf_data` / `js_ta_ptr`, the carried security fixes, and the browser hooks at the end of the file |
| `quickjs-libc.c` | SharedArrayBuffer memory for the host tools taken from the cage |
| `libregexp.c`, `libregexp-opcode.h` | 16-bit capture indices and the anchored-pattern speedup (see "Browser hooks" below) |
| `intl/*.c` | the default locale comes from `JS_GetDefaultLocale` instead of a fixed `en-US` |
| `qjs.c`, `run-test262.c` | no second `Intl`/`Temporal` initialisation; portability edits |
| `bcgen.c`, `meson.build` | ours entirely; upstream has neither |

`git diff <fork commit> HEAD -- src/quickjs-bellard` is the authoritative
list; every other file is upstream's and can be copied.

### Refresh log

| Date | Base | Notes |
|---|---|---|
| 2026-11-09 | `712e8c0` → `e5ac135` | Seventeen upstream commits, mostly memory-safety fixes (`ArrayBuffer` release callbacks run twice, #606; stale typed-array buffer length, #608; allocation failures in `JS_DefineProperty` and `add_fast_array_element`). The twelve carried fixes in the table above were added in the same change. |
| 2026-11-10 | `e5ac135` → `e585640` | The engine moved from the wrap into `src/quickjs-bellard/`. Five upstream commits: the object-spread fix for strings (#611), stricter Temporal time zone validation and three Intl fixes for #610 points 3, 4 and 11. |

The refreshes of the removed quickjs-ng and Lexbor forks, from 2026-06 to
2026-11, are in the git history only.

## Browser hooks in Bellard's engine

Declared in `src/quickjs-bellard/quickjs.h`:

| Hook | Purpose |
|---|---|
| `JS_EVAL_FLAG_HIDE_SOURCE` | compiles a script as the embedder's own code: its functions keep no source, so `Function.prototype.toString` reports them as native, and they are left out of a page's stack traces |
| `JS_IsHostAccess`, `JS_IsHostCaller`, `JS_IsEngineFunction` | tell the embedder's property accesses and callers from a page's: C functions made after the built-ins and hidden-source code are the embedder's |
| `JS_AddEnginePrivateName`, `JS_GetEnginePrivateAtom` | a name that stands for a private symbol when the embedder uses it, so the state kept on platform objects is neither visible nor writable from page script |
| `get_own_property_receiver` in `JSClassExoticMethods`, `JS_SetPropertyReceiver` | `[[Get]]` and `[[Set]]` with the receiver, for named access on a frame's window |
| `JS_NewCFunctionBrand`, `JS_SetCFunctionBrand` | a native function checks that `this` belongs to its interface before it runs |
| `JS_SetCanCompileStrings` | `eval()` and the `Function` constructors ask the browser first; a Content Security Policy without `'unsafe-eval'` refuses them with an `EvalError` |
| `JS_GetCallerRealm`, `JS_GetFunctionRealm`, `JS_GetPendingJobRealm` | the realm of the caller of the running native function, of a function, and of the next promise job |
| `JS_CloneCFunction` | a copy of a native function in another realm, for a frame's own copy of an interface |
| `JS_RepointArrayBuffer` | moves an imported `WebAssembly.Memory` buffer to the instance's memory |
| `JS_SetDefaultLocale`, `JS_GetDefaultLocale` | the locale `Intl` and the `toLocale*` functions use when a script names none |
| `JS_GetErrorFrames` | an error's stored stack |
| `JS_SetImmutablePrototype`, `JS_AtomIsArrayIndex` | exported as they are |
| `JS_IsDate`, `JS_IsRegExp`, `JS_IsMap`, `JS_IsSet`, `JS_IsProxy`, `JS_IsDataView`, `JS_IsArrayObject`, `JS_IsErrorObject`, `JS_GetBoxedPrimitiveKind` | exact class tests |
| `JS_SetHeapCage`, `JS_Cage*` | the heap cage and its shared heap |

The engine also hides the embedder's `__nd`/`__ns`/`__js` helpers on the
global object from a page's key enumeration, keeps the legacy RegExp
statics (`RegExp.$1` to `$9`, `lastMatch`, `lastParen`, `leftContext`,
`rightContext`, `input`), `Error.captureStackTrace`, and `caller` and
`arguments` on non-strict functions, and writes a native function whose
name is not an identifier as a computed key (`function get ["$&"]() ...`).

`JS_NewContext` adds `Intl` and `Temporal` itself, so that they count as
built-ins rather than as the embedder's functions. Do not call
`JS_AddIntrinsicTemporal` or `JS_AddIntrinsicIntl` again on such a context:
upstream's second initialisation leaks the context. `qjs.c` and
`run-test262.c` no longer do.

`libregexp.c` stores capture indices as 16 bits where upstream uses a byte,
so a pattern may have 65535 capture groups instead of 255 (upstream issue
[bellard/quickjs#624](https://github.com/bellard/quickjs/issues/624)):
bundlers emit patterns with more, such as rspack's CSS chunk test. The
compiled bytecode's header is 10 bytes, with a 16-bit capture count.

`quickjs-bellard-bcgen` (`bcgen.c`) compiles one engine script to
hidden-source bytecode; native builds embed `data/js/polyfills.js` and
`data/js/streaming.js` that way.

## The adapter: `src/ns_quickjs.h`

Engine code includes `"ns_quickjs.h"`, never `<quickjs.h>`. The adapter
(`src/ns_quickjs.h`, `src/ns_quickjs.c`) supplies what the engine and its
hooks leave out of the quickjs-ng-style API the binding uses:

- **Different signatures.** `JS_IsArray`, `JS_IsError` and `JS_IsBigInt`
  take no context in quickjs-ng; `JS_NewArrayBuffer` takes a realloc-style
  callback and a maximum length, and `JS_NewTypedArray` pads a short
  argument list with `undefined`. The promise-rejection tracker's
  `is_handled` argument is `ns_js_bool` and the interrupt handler's first
  argument is `ns_js_interrupt_arg`.
- **quickjs-ng-style additions rebuilt from public calls.** `JS_NewStringUTF16`,
  `JS_ThrowDOMException`, `JS_IsStrictEqual`, `JS_GetVersion`,
  `JS_ToNumber`, `JS_GetPropertyInt64`, `JS_FreezeObject`,
  `JS_IsRunningScript`, `JS_GetClassCount`, `JS_NewForwarder`, and
  `JS_EvalThis2`, which pads the source with the script's line and column
  offset because `JS_EvalThis` takes no position.

A binding change that calls a quickjs-ng-style function the engine lacks
fails to compile. Add the function to the engine
when it needs engine internals, or to `src/ns_quickjs.c` when public calls
are enough, rather than an `#ifdef` at the call site.

## Known differences from other engines

What a page may notice:

- **`error.stack`** has no leading `Name: message` line: it lists frames
  only, as Firefox and Safari do, not as V8 does. The
  engine's own tests and printers rely on upstream's format, so it is kept.
- **`Error.prepareStackTrace` and `Error.stackTraceLimit`** are absent, and
  `Error.captureStackTrace` ignores its second argument.
- **`ShadowRealm`** is absent. No shipping browser has it.
- **`Intl` and `Temporal`** are upstream's implementations, which upstream
  still calls experimental; their locale data is narrow.
- **Legacy RegExp statics** are updated by every match a page makes and
  are not invalidated after a subclass or cross-realm match.

## Heap cage and compressed pointers

The engine keeps every GC allocation inside a 4 GB reserved address range
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
  new native-pointer field only behind one of those tables.
  `JS_SetOpaque` picks the encoding by class id: a cage offset for
  a built-in class, an external pointer table index for any other, which
  includes the `Intl` and `Temporal` classes.
- `JSRuntime`, `JSContext`, `class_array`, the atom table and the shape
  hash live outside the cage in trusted blocks; a new field that points
  from a cage object to one of them must be a table index, not a pointer.
- An ArrayBuffer's backing store must be cage memory: `JS_NewArrayBuffer`
  with a foreign buffer throws. Take the memory from `JS_CageMalloc`
  (`ns_js_cage_shared_malloc` in browser code).
- `scripts/quickjs-cage-rewrite.py` lists the engine's compressed fields
  and re-applies or checks their accesses (`--apply`, `--check`).

`docs/heap-hardening.md` has the design, the inventory of what is still
raw, and the measurements.

### The hardening stages in this engine

The stages were first built in the quickjs-ng fork and ported. Every one
of them is in Bellard's engine:

| Stage | In Bellard's engine |
|---|---|
| 1a cage | `quickjs-cage.c`. `JS_NewRuntime()` gives each runtime a heap in the process-wide 4 GB cage. The engine's small-block arena, whose bookkeeping is in-band, steps aside while the cage is active. Blocks are released by address, never by the header inside the cage. |
| 2a buffers | `JSArrayBuffer.data` is a compressed field, so a foreign backing store cannot be stored; `JS_NewArrayBuffer` and the SharedArrayBuffer allocation hook refuse one with an exception. Typed-array element pointers are masked into the cage on every read. |
| 2b tables | Host opaques go through the tagged external pointer table. C functions, data functions, jobs, C-module init functions and ArrayBuffer free hooks are indexes into the typed code pointer table; the entry holds `cproto`, `magic` and the argument count. Auto-init properties name their function-list entry by index. |
| 3p trusted structures | `JSRuntime`, `JSContext`, the class table, the atom table and hash, the shape hash and async Atomics waiters are trusted blocks. Realm references in heap objects are context-table indexes checked against the runtime. Class and atom lookups reachable from heap data are bounded. |
| 3a.1 caged values | `JS_CAGE_BOXING`: upstream already NaN-boxes values on x86-64 with a 47-bit pointer; the caged layout uses a 32-bit tag shift and a 32-bit cage offset. `JSObject` stays at 56 bytes. |
| 3a.2 compressed fields | the `FIELDS` table in `scripts/quickjs-cage-rewrite.py`, plus the Map hash table, whose buckets are compressed slots. |
| safe-linked lists | `list.h` checks both neighbours before every insert and unlink. |
| 3b trusted bytecode | planned. |

Where Bellard's code is shaped differently from the fork the design was
first written for:

- `JS_SetOpaque` serves built-in and host classes alike, so it picks the
  encoding by class id. `Intl` and `Temporal` register ordinary class ids
  and need no tag of their own.
- A property's accessor pair and auto-init record are separate
  allocations (`JSPropertyGetSet`, `JSPropertyAutoInit`), reached through
  compressed fields. The auto-init record holds a context index, an id
  that is bounded before it selects a function, and an opaque that is a
  function-list index or a cage offset.
- A zero-size allocation gets a real cage block while the cage is active,
  because upstream's shared zero-size block lies in the runtime, outside
  the cage, and could not be stored in a compressed field.
- There are no weak-reference chains to convert: upstream counts weak
  references on the object instead.

Of the hardening review findings in `heap-hardening.md`, M1 does not
apply to this engine: its engine-private name table and bitmap are
trusted blocks outside the cage, and the brand of a native function lives
in its code pointer table entry. M2 applies.

### Cost

When the hardening was ported (2026-11), an allocation-heavy
microbenchmark (3 million object and array allocations, a 300 000-entry
Map, 200 000 string concatenations) took about 570 to 650 ms on the
hardened engine against 360 to 480 ms on unmodified upstream, on one
4-core container. Most of that is the arena bypass: with the cage active
every block comes from the cage allocator instead of upstream's in-band
arena.

A full parallel test262 run is several times slower than on upstream,
with most of the time in the kernel: the runner creates and destroys a
runtime per test, and each one commits and decommits cage chunks. A
renderer creates a handful of runtimes in its lifetime.

## Checking a change

Build with no warnings, GCC and Clang, and run the smoke test:

```sh
meson setup builddir && meson compile -C builddir
./scripts/dev.sh smoke
```

After a change to Bellard's engine, run its tests through the host tool:

```sh
cd src/quickjs-bellard
Q=../../builddir/src/quickjs-bellard/qjs-bellard
for t in test_closure test_language test_loop test_bigint; do $Q tests/$t.js; done
$Q --std tests/test_builtin.js && $Q --std tests/test_std.js && $Q --std tests/test_worker.js
$Q --experimental tests/test_intl.js
```

and test262, which finds faults live pages do not:

```sh
ninja -C builddir src/quickjs-bellard/run-test262-bellard
```

Clone tc39/test262 at the commit upstream's `Makefile` names
(`TEST262_COMMIT`), apply `tests/test262.patch`, link it as `test262` next
to `test262.conf` and run `run-test262-bellard -t -m -c test262.conf -a`.
The expected result is the content of `test262_errors.txt`.
`scripts/test262-run.sh` does the same against test262's current `main`,
fetched into `src/quickjs-bellard/test262`; `-u` rewrites
`test262_errors.txt` from the result. A pointer
stored outside the cage shows up as an abort naming it, which is the
intended failure mode.
