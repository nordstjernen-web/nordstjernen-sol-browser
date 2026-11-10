# Bellard's QuickJS as the default engine

Since 1.0.47 the default JavaScript engine is an in-tree fork of Fabrice
Bellard's [QuickJS](https://github.com/bellard/quickjs) in
`src/quickjs-bellard/`. The quickjs-ng fork in `src/quickjs/` stays in the
tree and is one configure flag away:

```sh
meson setup builddir                        # Bellard's QuickJS
meson setup builddir -Dquickjs=quickjs-ng   # the quickjs-ng fork
```

This page records what changed and what is still different. The day to
day reference is [`quickjs.md`](quickjs.md); the hardening design is
[`heap-hardening.md`](heap-hardening.md).

## What changed

**The engine moved into the tree.** It was fetched at configure time by
`subprojects/quickjs.wrap` with twelve patches applied on every fetch.
The heap hardening rewrites about 800 field accesses in `quickjs.c`, which
a patch series cannot carry, so the engine is a fork now, taken from
upstream master `e585640` (2026-11-10) with the twelve patches applied to
the sources. The wrap, `subprojects/packagefiles/quickjs*` and
`scripts/bump-bellard-quickjs.sh` are gone; a refresh is a merge
([`quickjs.md`](quickjs.md), "Moving to a newer upstream commit").

**The default flipped.** `-Dquickjs` defaults to `quickjs`. CI, the
packaging scripts and the Android build take the default, so they build
Bellard's engine; `-Dquickjs=quickjs-ng` is built locally.

**Heap hardening was ported.** Every stage the quickjs-ng fork has is in
Bellard's engine:

| Stage | In Bellard's engine |
|---|---|
| 1a cage | `quickjs-cage.c`, shared with the fork's design. `JS_NewRuntime()` gives each runtime a heap in the process-wide 4 GB cage. The engine's small-block arena, whose bookkeeping is in-band, steps aside while the cage is active. Blocks are released by address, never by the header inside the cage. |
| 2a buffers | `JSArrayBuffer.data` is a compressed field, so a foreign backing store cannot be stored; `JS_NewArrayBuffer` and the SharedArrayBuffer allocation hook refuse one with an exception. Typed-array element pointers are masked into the cage on every read. |
| 2b tables | Host opaques go through the tagged external pointer table. C functions, data functions, jobs, C-module init functions and ArrayBuffer free hooks are indexes into the typed code pointer table; the entry holds `cproto`, `magic` and the argument count. Auto-init properties name their function-list entry by index. |
| 3p trusted structures | `JSRuntime`, `JSContext`, the class table, the atom table and hash, the shape hash and async Atomics waiters are trusted blocks. Realm references in heap objects are context-table indexes checked against the runtime. Class and atom lookups reachable from heap data are bounded. |
| 3a.1 caged values | `JS_CAGE_BOXING`: upstream already NaN-boxes values on x86-64 with a 47-bit pointer; the caged layout uses a 32-bit tag shift and a 32-bit cage offset. `JSObject` stays at 56 bytes. |
| 3a.2 compressed fields | 63 entries in `BELLARD_FIELDS` (`scripts/quickjs-cage-rewrite.py`), plus the Map hash table, whose buckets are compressed slots. |
| safe-linked lists | `list.h` checks both neighbours before every insert and unlink. |
| 3b trusted bytecode | planned, as in the fork. |

Differences from the fork's implementation, where Bellard's code is
shaped differently:

- `JS_SetOpaque` serves built-in and host classes alike in Bellard's
  engine, so it picks the encoding by class id. `Intl` and `Temporal`
  register ordinary class ids and need no tag of their own.
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

**Browser hooks were ported.** The CSP `eval` hook, the three realm
queries, native-function cloning, `JS_RepointArrayBuffer` and exact class
tests are engine functions now instead of approximations in the adapter
([`quickjs.md`](quickjs.md), "Browser hooks in Bellard's engine"). That
closes these former differences: `eval` under a Content Security Policy,
the calling realm, promise jobs in frames, native functions in frames,
views on an imported `WebAssembly.Memory`, and the string round trip in
indexed access on host objects.

**`Intl` and `Temporal` are built.** Upstream's own implementations
(`CONFIG_TEMPORAL_INTL`) are compiled and added to every context.

## Verification

| Check | Result |
|---|---|
| test262 (`run-test262 -a`, commit pinned by upstream's Makefile) | 19 errors of 53021, the same 19 as unmodified upstream, at each stage: cage and caged values; compressed fields; pointer tables and bounded lookups |
| engine tests (`test_closure`, `test_language`, `test_loop`, `test_bigint`, `test_builtin`, `test_std`, `test_worker`, `test_intl`) | pass |
| `scripts/dev.sh smoke` | all ten fixtures match their baselines |
| DOM dump of the 65 pages in `data/fixtures/`, `data/render-tests/` and `docs/demos/` | identical to the quickjs-ng build |
| CSP page calling `eval` and `new Function` | `EvalError` on both engines |

Not covered: live sites (the build machine had no outbound web access),
the GTK shell under a display, and Windows, macOS and Android, which CI
builds.

## Cost

An allocation-heavy microbenchmark (3 million object and array
allocations, a 300 000-entry Map, 200 000 string concatenations) takes
about 570 to 650 ms on the hardened engine against 360 to 480 ms on
unmodified upstream, on one 4-core container. Most of that is the arena
bypass: with the cage active every block comes from the cage allocator
instead of upstream's in-band arena. The quickjs-ng fork pays the same
kind of cost (`heap-hardening.md`, "Benchmarks"). Sitebench against live
sites has not been run on this engine yet.

A full parallel test262 run is several times slower than on upstream,
with most of the time in the kernel: the runner creates and destroys a
runtime per test, and each one commits and decommits cage chunks. A
renderer creates a handful of runtimes in its lifetime.

## Still different from the quickjs-ng build

Listed in [`quickjs.md`](quickjs.md), "Known differences on Bellard's
engine": the receiver-aware property hook, host access and engine-private
names, `this` brands, a few legacy language features, hidden engine
source, engine script bytecode, the default locale and `ShadowRealm`.
The hardening review findings M1 and M2 in `heap-hardening.md` apply to
both engines where the feature exists; M1 (engine-private names) has no
counterpart in Bellard's engine yet.
