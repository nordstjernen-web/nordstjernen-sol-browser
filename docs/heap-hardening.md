# JavaScript heap hardening

This document tracks the work that makes a memory-safety bug in the
JavaScript engine or its bindings harder to turn into control of the
renderer process. The model is V8's heap sandbox: assume an attacker who
can already read and write arbitrary memory *inside* the JavaScript heap,
and arrange the heap so that this is not enough to reach memory outside
it. Until stage 3b lands this is **exploit-cost hardening**, not a
security boundary: each stage removes a class of primitive an exploit
relies on, but an attacker who corrupts the bytecode the interpreter
trusts can still size and index a native stack frame. Only a trusted
space for bytecode and saved frames makes the heap a boundary that holds
on its own, and only then is it fair to call it a sandbox.

Nordstjernen's position differs from V8's in ways that make the work
smaller: QuickJS is an interpreter, so the renderer never maps executable
memory, and with WebGL and WebGPU switched off the seccomp filter refuses
`PROT_EXEC` mappings outright (`src/security.c`); the heap is small; and
every host object reaches the heap through one opaque pointer API. It
also differs in a way that makes it harder: the binding layer
(`src/js.c`) is as large as the engine.

Each change is benchmarked before it merges and kept as small and
isolated from upstream quickjs-ng as the design allows, so security
fixes from upstream keep merging cleanly into `src/quickjs/`.

## Status

| Stage | What | State |
|---|---|---|
| 1a | Separate allocation partition for the JS heap (the cage) | done |
| 1b | Credential-less renderer broker | not started, see below |
| 2a | Bounded, cage-relative ArrayBuffer and TypedArray pointers | done |
| 2b | Tagged external pointer table and code pointer table | done |
| 3p | Trusted runtime structures and context table | done |
| 3a.1 | Caged values: 8-byte `JSValue` holding a 32-bit cage offset | done |
| 3a.2 | Compressed pointer fields inside heap structures | batches 1 to 7 done; module records open |
| 3b | Trusted space for bytecode and saved frames | planned |

Stage 1b, moving cookies, HSTS, the HTTP cache and storage behind the
shell so a compromised renderer holds no credentials, is independent of
the heap stages and remains the complementary architectural step that
`docs/tab-isolation.md` already lists. It limits what a compromise is
worth; the stages here limit how a bug becomes a compromise.

## What is in place

The invariants below are what the engine and the bindings must keep.

**The cage** (`src/quickjs/quickjs-cage.c`). `JS_NewRuntime()` reserves
one 4 GB range per process on first use (halving to 1 GB when address
space is short), aligned to its size, with 32 GB inaccessible guard
regions on both sides. Each runtime (page, worker, extension host) gets
its own lock-free heap inside it. Allocations up to 32 KB come from
size-classed 64 KB slabs, larger ones from runs of slabs or 1 MB chunks;
freed chunks beyond a small cache are decommitted, so a stale pointer
into them faults. **No allocator bookkeeping lives inside the cage**:
chunk and slab metadata, bitmaps and free lists are outside it, found
through a side table by chunk number, and the allocator aborts on a free
it did not hand out, a misaligned free or a double free. The fork's own
small-block arena steps aside while the cage is active. The first chunk
is never handed out, so offset zero means `NULL`. The engine refuses to
create a runtime without a cage; AddressSanitizer and 32-bit builds
compile the classic layouts and use the process heap, and
`-Dquickjs=quickjs` (Bellard's original engine) has no cage at all.
`src/js_cage.c` wraps it for browser code (`ns_js_runtime_new`,
`ns_js_cage_shared_malloc`) and logs the reservation under
`G_MESSAGES_DEBUG=all`.

**Buffers.** Every ArrayBuffer backing store lives inside the cage: the
engine refuses a foreign store (`JS_NewArrayBuffer` with an external
buffer, `JS_RepointArrayBuffer`, a SharedArrayBuffer allocation hook
returning out-of-cage memory). WebAssembly memory comes from a locked
shared heap inside the cage (`src/wasm.c`), and WebGPU `getMappedRange()`
returns a cage copy written back on `unmap()` (`src/webgpu.c`). Every
read of a buffer data pointer or the cached typed-array element pointer
is masked into the cage (`js_abuf_data()`, `js_ta_ptr()`), and with
32-bit lengths and offsets the furthest reach is the trailing guard.

**Tables outside the cage** (`src/quickjs/quickjs-hardening.h`).

- External pointer table: `JS_SetOpaque` on a host class stores the
  pointer in an entry tagged with the class id; the object holds the
  index, and `JS_GetOpaque` returns `NULL` on a tag mismatch. The Intl
  classes, whose structures come from libc, use it too under their own
  tag.
- Code pointer table: every native function pointer a heap object used
  to hold (C functions, function-data and closure records, their
  finalizers, the ArrayBuffer reallocation hook, job functions, C-module
  init functions) is an index into an append-only table.
- Context table: `JSRuntime`, every `JSContext` and the class table are
  allocated from trusted memory outside the cage, and every heap field
  that named a context (function realms, jobs, finalization records,
  Atomics waiters, ShadowRealm) holds a masked index instead.

**Caged values.** Under `JS_CAGE_BOXING` (default on 64-bit builds
without ASan) a `JSValue` is 8 bytes whose pointer payload is a 32-bit
cage offset, so no property slot, array element, stack slot, constant
pool entry or argument can name memory outside the cage.

**Compressed fields.** Pointer fields inside cage structures are
declared `JS_CPTR(T)` (`src/quickjs/quickjs-cptr.h`), read through
`js_cload(T, field)` and written through `js_cstore(field, ptr)`. The
encoder **fails closed**: a pointer from outside the cage aborts with a
message naming it instead of being truncated into a random in-cage
address. Converted so far: object shape, property array, prototype and
shape hash chain; fast-array values, closure variables and home object;
the bytecode arrays and source text; weak-reference chains and records;
string-slice parents; every member of the object's opaque union and the
function's bytecode pointer; typed-array back pointers; regexp pattern
and bytecode; Map hash buckets, record owners and iterator cursors;
promise resolution state; async-generator back pointers; iterator-helper
buffers.

**Safe-linked lists.** The `list_head` links stay 64-bit because the
runtime's GC list threads both cage objects and the trusted contexts,
but `list.h` checks on every insert and unlink that both neighbours
point at each other before it writes. An in-cage attacker cannot plant
the matching back pointer outside the cage, so the classic unlink write
primitive is gone.

## What is still open

The rows below are the raw pointers or trusted data a write primitive
inside the cage can still corrupt. Everything not listed is either
converted or is trusted memory pointing into the cage, which the model
allows (`ctx->class_proto`, `rt->atom_array`, `ns_node.js_wrapper` and
the other embedder structures).

| Where | What | Reach | Plan |
|---|---|---|---|
| `JSFunctionBytecode` | opcodes, `cpool`, `closure_var`, `vardefs`, `stack_size`, `var_count`, `arg_count` | sizes and indexes the interpreter trusts for its native frame | **3b** |
| `JSAsyncFunctionState` (generators, async functions) | saved `JSStackFrame`: `cur_pc`, `arg_buf`, `var_buf`, `var_refs`, `prev_frame` | native stack layout | **3b** |
| `JSVarRef` | `pvalue`, `stack_frame` of an open reference | points at a live native stack frame or a saved frame | **3b**, with the frames |
| `JSModuleDef`, `JSReqModuleEntry`, `JSExportEntry` | `req_module_entries`, `export_entries`, `star_export_entries`, `import_entries`, `stack_prev`, `async_parent_modules`, `cycle_root`, `module`, `local.var_ref` | raw pointers in cage structures; about 107 sites, many through `js_resize_array` | 3a.2 batch 8 |
| regexp bytecode | jump offsets and register indexes `lre_exec` reads from the in-cage bytecode string | reads and capture writes relative to cage buffers; the guard regions bound the reach, but the interpreter has not been reviewed against a corrupted program | review with 3b |
| `JSAtomicsWaiter.async_rt` | `JSRuntime *`, tested and compared, never dereferenced | none today | low value |
| `JSRegExp.realm` | `JSContext *`, compared, never dereferenced | none today | low value |

### Next: batch 8, module records

The module structures are cage allocations full of raw pointers:
corrupting `req_module_entries` or an entry's `module` hands module
linking and evaluation an arbitrary address to read and write through.
Convert them the way the other batches went: declare the fields
`JS_CPTR(T)`, add them to `scripts/quickjs-cage-rewrite.py`, run
`--apply`, and take the address-of sites (`js_resize_array(ctx, (void
**)&m->export_entries, …)`) by hand through a local, as batch 7 did for
the iterator-helper buffer. Verify with the module directories of
test262 and a site that loads ES modules.

### Stage 3b: trusted bytecode space (plan)

Allocate `JSFunctionBytecode`, its opcode buffer, closure tables and the
saved frames of generators and async functions from memory outside the
cage, reached through an indirect table, so the interpreter sizes and
indexes its native stack frame only from data an in-cage attacker cannot
reach.

- `JSFunctionBytecode` is a GC object with its reference count in the
  allocation header, so the trusted block path that already carries
  `JSRuntime` and `JSContext` can carry it too; its `cpool` values and
  realm index may keep pointing into the cage and the context table.
- Function objects reach their bytecode through `u.func.function_bytecode`
  (a cage offset today); that becomes an index into a trusted bytecode
  table, decoded with a mask like `js_ctx_get`.
- Bytecode also travels as a value: `JS_TAG_FUNCTION_BYTECODE` values sit
  in constant pools and module records, and under cage boxing a value's
  payload is a cage offset. Make the payload for that tag the table
  index and route the handful of bytecode-tag sites (four `JS_MKPTR`,
  five `case` labels in GC marking, freeing and the constant-pool reader)
  through a tag-aware getter. The generic macros stay tag-agnostic and
  fast. The alternative, an in-cage handle object standing in for the
  bytecode in values, keeps the macros untouched but adds an allocation
  per function.
- Generator and async frames embed a `JSStackFrame`; they move to trusted
  blocks with the same indirection, which also takes the open `JSVarRef`
  pointers into them out of reach. The interpreter's `alloca` sizing then
  reads `stack_size`, `var_count` and `arg_count` from trusted memory
  only.

### Performance follow-up

Caging values costs about 2 to 3 % main-thread CPU on script-heavy sites
(stage 3a.1, below), from the base-plus-offset decode on every pointer
value. Caching the cage base in a local inside the interpreter loop,
where most decodes happen, is the obvious thing to try; the compressed
fields cost nothing measurable on top.

## Lessons that apply to every batch

- When a slot changes encoding, grep every reader of it, including the
  generic opaque getters. Batch 6 stored ShadowRealm's context as a
  context-table index while `JS_GetOpaque` still decoded the slot as a
  cage offset; `JS_GetOpaque` and `JS_GetAnyOpaque` now pick the
  encoding by class in one place.
- Anything stored in a compressed field must be a cage allocation. Libc
  structures (the Intl case) go through the external pointer table, and
  sentinels (the cleared `WeakRef` marker) are cage allocations.
- The rewrite script skips comments and cannot see through `&field`:
  address-taking sites need a local that is stored back, and a
  `sizeof(x->field[0])` on a compressed field must name the element type.
- Run test262 as well as browsing captures: the full run found faults
  live sites had not.
- A full parallel test262 run can report `InternalError: out of memory`
  from `staging/sm/String/replace-math.js`, because the runner's threads
  share one 4 GB cage. Run alone it peaks near 2 GB and passes; a
  renderer runs one page per process.

## Maintaining the hardening across upstream refreshes

The fork-local code is kept out of `quickjs.c` where it can be, and the
rewrites that cannot be are reproducible:

- `src/quickjs/quickjs-cptr.h` holds the compressed-field machinery and
  the fail-closed encoder, `src/quickjs/quickjs-hardening.h` the cage
  masking and the three tables. `quickjs.c` includes each at one line.
  `quickjs-cage.c` is a separate file, and `list.h` carries the
  safe-linking checks.
- `scripts/quickjs-cage-rewrite.py` is the single list of compressed
  fields. `--check` reports every raw access to one of them outside
  comments and exits non-zero; `--apply` rewrites them in place,
  idempotently, and reports the address-taking and slot-move sites that
  need a hand. After a refresh: 3-way merge `quickjs.c`, declare any new
  pointer field in a heap structure `JS_CPTR(T)` and add it to the
  script's table, run `--apply`, build, run test262 and a settled
  live-site capture. A pointer that escaped the rules fails loudly: the
  encoder aborts on an out-of-cage pointer and the lists abort on a bad
  link.
- The trusted block path, the `JS_NewRuntime` cage path, the Intl and
  WeakRef sentinel handling, the bytecode-tag sites and the few hand
  conversions the script reports are listed in
  `docs/vendored-engine-updates.md` for the person doing the refresh.

## Benchmarks

Sitebench (`scripts/sitebench/ab.sh`, old build against new on google,
anthropic, bbc, cnn and guardian, `RUNS=2 VISUAL_RUNS=1` on a 4-core
container, 2025-11-09). Single-site deltas under about 10 % are noise;
the aggregate is what to read.

| Stage | Main CPU ÷ Chrome, before → after | Memory ÷ Chrome, before → after | Note |
|---|---|---|---|
| 1a cage | 3.71× → 3.84× | 0.26× → 0.27× | RSS +0.5 to 2 % from slab rounding and outside metadata |
| 2a masked buffers | 3.40× → 3.52× | 0.27× → 0.27× | an AND and an OR per buffer access |
| 2b tables + arena bypass | 3.62× → 3.63× | 0.26× → 0.26× | arena bypass costs ~10 % on pure allocation churn, invisible in page loads |
| 3p trusted contexts | 3.99× → 3.98× | 0.29× → 0.30× | |
| 3a.1 caged values | 3.87× → 3.97× | 0.30× → 0.30× | the one visible cost, 2 to 3 % on script-heavy sites |
| 3a.2 batches 2 and 3 | 3.54× → 3.55× | 0.26× → 0.25× | |
| 3a.2 batch 4 | 3.52× → 3.47× | 0.27× → 0.27× | |
| 3a.2 batch 5 | 3.43× → 3.34× | 0.26× → 0.26× | |
| 3a.2 batch 6 | 3.98× → 3.88× | 0.28× → 0.28× | |
| 3a.2 batch 1 (safe linking) | 3.68× → 3.60× | 0.28× → 0.27× | |

The allocation stress script (200 000 objects, 64 typed arrays, a 40 MB
ArrayBuffer, a 200 KB concatenated string, a 100 000-entry Map, then
small-array churn) went from 0.32 s / 177 MB with the cage alone to
0.37 s / 174 MB after batch 6; `qjs -d` reports `JSObject` at 72 bytes.

Batch 7 touches no interpreter hot path, so it was measured with `qjs`
microbenchmarks over the code it changed (Windows, MSYS2 build, three
interleaved runs each, milliseconds, 2026-11-09):

| Benchmark | before | after |
|---|---|---|
| 400 000 `RegExp.exec` | 1010 to 1019 | 956 to 998 |
| 300 global `replace` + `split` over 8 KB | 226 to 252 | 230 to 240 |
| 300 000 typed-array views, `.buffer`, `subarray` | 448 to 471 | 450 to 481 |
| 300 000-entry Map fill, iterate, look up | 648 to 662 | 611 to 635 |
| 200 000 WeakMap entries and WeakRefs | 193 to 200 | 190 to 193 |
| 200 000 resolved promises | 404 to 495 | 366 to 406 |
| mixed churn (objects, Map, typed arrays, strings, regexps, helpers) | 605 to 610 | 587 to 595 |

Flat; the extra decode per access is lost in the noise.
