# JavaScript heap hardening

This document tracks the work that makes a memory-safety bug in the
JavaScript engine or its bindings harder to turn into control of the
renderer process. The model is V8's heap sandbox: assume an attacker who
can already read and write arbitrary memory *inside* the JavaScript heap,
and arrange the heap so that this is not enough to reach memory outside
it. Until the last stage lands, the stages here are **exploit-cost
hardening**, not a security boundary: each removes a class of primitive an
exploit relies on, but a determined attacker with a second bug in the
right place can still escape. Only pointer compression together with a
trusted space for bytecode (stage 3) makes the heap a boundary that holds
on its own, and only then is it fair to call it a sandbox.

Nordstjernen's position differs from V8's in ways that make the work
smaller: QuickJS is an interpreter, so the renderer never maps executable
memory, and with WebGL and WebGPU switched off the seccomp filter refuses
`PROT_EXEC` mappings outright (`src/security.c`); the heap is small; the
engine exposes a pluggable
allocator; and every host object reaches the heap through one opaque
pointer API. It also differs in a way that makes it harder: the binding
layer (`src/js.c`) is as large as the engine and holds raw pointers in
both directions.

Each stage is benchmarked with `scripts/sitebench/ab.sh` (old build
against new build on live sites) before it merges, and kept as small and
isolated from upstream quickjs-ng as the design allows, so security fixes
from upstream keep merging cleanly into `src/quickjs/`.

## Status

| Stage | What | State |
|---|---|---|
| 1a | Separate allocation partition for the JS heap (the cage) | **done** (`src/js_cage.c`) |
| 1b | Credential-less renderer broker | skipped for now, see below |
| 2a | Bounded, cage-relative ArrayBuffer and TypedArray pointers | **done** |
| 2b | Tagged external pointer table and code pointer table | **done** |
| 2c | Inventory of raw host and function pointers left in the heap | **done**, kept current below |
| 3p | Trusted runtime structures and context table (preparation for 3a and 3b) | **done** |
| 3a.1 | Caged values: 8-byte `JSValue` holding a 32-bit cage offset, cage owned by the engine | **done** |
| 3a.2 | Compressed pointer fields inside heap structures | **batches 2 to 6 done**: shape, property array, prototype, shape hash chain, fast-array values, closure variables, home object, bytecode arrays, weak-reference chains, string-slice parents, every internal class structure behind the opaque union and the bytecode pointer of functions. Batch 1 (GC list links) remains |
| 3b | Trusted space for bytecode and saved frames | planned |

Stage 1b, moving cookies, HSTS, the HTTP cache and storage behind the
shell so a compromised renderer holds no credentials, is independent of
the heap stages and remains the complementary architectural step that
`docs/tab-isolation.md` already lists. It limits what a compromise is
worth; the stages here limit how a bug becomes a compromise.

## Stage 1a: the cage

The cage reserves one range of virtual address space per renderer
process at the first runtime creation and serves every QuickJS
allocation from it. It started life in `src/js_cage.c`, plugged in
through the public `JS_NewRuntime2` allocator hook with no engine
change; since stage 3a.1 it lives in the engine itself
(`src/quickjs/quickjs-cage.c`, GLib-free, used by `JS_NewRuntime()`), so
the in-tree `qjs`/`qjsc` host tools and test262 run the real layout, and
`src/js_cage.c` is a thin wrapper that logs the reservation and exposes
the shared cage heap to `src/wasm.c` and `src/webgpu.c`. The fork's own
small-block arena allocator
(`src/quickjs/quickjs.c`) steps aside while a cage is active: its arena
headers, free lists and list links live inside the blocks it manages,
so a corrupted heap could have steered `js_arena_free` into an unlink
write outside the cage. With the cage on, every allocation goes to the
cage allocator and its out-of-cage bookkeeping; without one, the arena
works as before.

Layout:

- The reservation is 4 GB, the most a 32-bit offset can address, halving
  down to 1 GB when the address space is short, aligned to its own size
  so the offset mask is a plain bit-and. Inaccessible guard regions of
  32 GB sit on both sides, and the first chunk is never handed out so
  offset zero stays distinguishable from a pointer. Nothing is committed
  until used. (Stage 1a reserved 64 GB with 8 GB guards; stage 2a raised
  the guards to 32 GB for the bounded-length arithmetic; stage 3a.1
  brought the cage down to the 4 GB a boxed value can name.)
- The range is cut into 1 MB chunks; chunks into 64 KB slabs. Allocations
  up to 32 KB come from size-classed slabs (40 classes, four per
  doubling), allocations up to 1 MB from runs of slabs, and larger ones
  from runs of chunks. Freed chunks are decommitted (`madvise` plus
  `PROT_NONE`, `VirtualFree` on Windows) beyond a small committed cache,
  so a stale pointer into a released chunk faults instead of reading.
- Each runtime (page, dedicated worker, extension host) owns a heap of its
  own inside the shared cage, so the hot allocation path takes no lock;
  only chunk acquisition and release do.

Design rule, the same one V8 arrived at after moving its chunk metadata
out of the sandbox: **no allocator bookkeeping lives inside the cage.**
Chunk and slab metadata, bitmaps and free lists are ordinary process
allocations outside the range, looked up through a side table indexed by
chunk number. An attacker who controls the heap contents cannot therefore
make the allocator hand out an address of their choosing, and a corrupted
heap cannot forge a free-list link, a size class or a run length. The
allocator aborts on a free of a pointer it did not hand out, a misaligned
free or a double free.

What stage 1a buys on its own:

- JavaScript objects can no longer be groomed next to DOM nodes, network
  buffers, layout state or the IPC framing. A use-after-free in the
  engine cannot be reclaimed by an attacker-shaped DOM allocation, and
  the reverse holds.
- Guard regions and decommitted chunks turn a class of stale reads into
  crashes.
- Every later stage needs a cage base and a power-of-two size to mask
  against; they now exist (`ns_js_cage_base`, `ns_js_cage_size`).

What it does not buy: heap-internal pointers are still raw 64-bit
addresses, so a corrupted object pointer still reaches anywhere. That is
the subject of stages 2 and 3.

Switches: none at runtime any more. Since stage 3a.1 a boxed value can
only name cage memory, so the engine refuses to create a runtime when
the reservation fails instead of falling back. Builds with
AddressSanitizer and 32-bit builds compile the classic value layouts and
use the process heap. The cage prints its base and size under
`G_MESSAGES_DEBUG=all`. The `-Dquickjs=quickjs` configuration, Bellard's
original engine, has no cage at all.

Measurements (headless, this container, median of three):

| Run | Wall time | Peak RSS |
|---|---|---|
| Allocation stress script, process heap | 0.37 s | 175.7 MB |
| Allocation stress script, cage | 0.32 s | 177.3 MB |

The stress script builds 200 000 objects, 64 typed arrays, a 40 MB
ArrayBuffer, a 200 KB string by concatenation and a 100 000-entry Map,
then churns small arrays. Sitebench results are recorded in the
*Benchmarks* section below as stages land.

## Stage 2: buffers and pointer tables

Stage 2a (done) masks every read of an ArrayBuffer data pointer and of
the cached typed-array element pointer into the cage: the engine keeps
the fields as they are and decodes them through `js_abuf_data()` and
`js_ta_ptr()` as `cage_base | (pointer & (cage_size - 1))`, so a forged
buffer with an arbitrary address still lands inside the cage, and with
lengths and offsets of at most 32 bits and elements of at most 8 bytes
the furthest reach is 32 GB past the end, which is exactly the trailing
guard region. The embedder hands the engine the cage with
`JS_SetHeapCage()`; without a cage the mask is the identity, so the
fallback path and the ASan build run the unmodified arithmetic. The
engine change is one 20-line block plus mechanical substitutions at the
read sites (17 buffer reads, 67 typed-array pointer reads), with no
change to the writers, which keeps the diff against upstream small and
re-applicable.

The rule that makes this sound is that **every ArrayBuffer backing store
lives inside the cage**. The engine now refuses a foreign backing store
outside it (`JS_NewArrayBuffer` with an external buffer, and
`JS_RepointArrayBuffer`). Two embedder paths had to change to follow the
rule: WebAssembly linear memory and the runtime's own allocations come
from a locked shared heap inside the cage through WAMR's allocator hooks
(`WASM_MEM_ALLOC_WITH_USAGE`, `src/wasm.c`), with linear memory zeroed on
allocation and growth; and WebGPU `getMappedRange()` returns a cage copy
of the mapped range that is written back to the GPU buffer on `unmap()`
for write mappings (`src/webgpu.c`) instead of aliasing wgpu-native's
memory.

Stage 2b (done) adds two tables that live outside the cage, both in one
block of `src/quickjs/quickjs.c` next to the cage helpers:

- **External pointer table.** `JS_SetOpaque` on a host class (any class
  id at or above `JS_CLASS_INIT_COUNT`, which is every class the browser
  registers) stores the pointer in a table entry tagged with the class
  id and leaves only the entry index in the object. `JS_GetOpaque` and
  `JS_GetAnyOpaque` look the index up, masked to the table's capacity,
  and return `NULL` when the tag does not match the class the caller
  asked for, so a corrupted index can at most name another live object
  of the same class and a forged pointer is never dereferenced. The
  entry is released when the object is freed, after its finalizer ran.
  The table is a two-level array of 16 M entries whose segments never
  move, so reads need no lock; allocation and release take a spinlock
  shared with the code table. The engine's own classes keep their
  internal structs in the object as before, since those point into the
  cage anyway.
- **Code pointer table.** Native function pointers stored in heap objects
  are interned into an append-only table and the object holds the
  index: the C function behind every `JS_NewCFunction*` object, the
  function and data-record function of `JS_NewCFunctionData`, the
  function and finalizer of `JS_NewCClosure`, and the ArrayBuffer
  reallocation hook. A corrupted index selects another registered
  function at worst, never an arbitrary address. Interning deduplicates
  through a hash table under the same spinlock; lookups are a masked
  two-level array read.

Both sit behind the existing API, so the call sites in `src/js.c`,
`src/webgpu.c`, `src/wasm.c` and the canvas code did not change; the
engine diff is the table block, four struct fields and about twenty
call-site lines.

### Stage 2c: raw pointers still in the heap

What a write primitive inside the cage can still corrupt into something
dangerous, after stages 1a to 2b. Items marked *3a* fall to pointer
compression, *3b* to trusted runtime structures and bytecode space, and
*2d* are small follow-ups that fit the existing tables.

| Where | Pointer | Reach | Stage |
|---|---|---|---|
| `JSRuntime` | `mf` allocator hooks, `class_array[]` finalizer / `gc_mark` / `call` / `exotic` methods, interrupt handler, promise-rejection tracker, module loader, `sab_funcs` | trusted memory outside the cage | done (3p) |
| `JSContext` | `rt`, realm tables, class prototypes, `host_functions` policy bits | trusted memory outside the cage, reached from the heap only through the context table | done (3p) |
| `JSJobEntry` | `ctx`, `job_func` | context-table index, code-table index | done (3p) |
| `JSModuleDef.init_func`, `JSFinRecEntry.ctx`, `JSFinalizationRegistryData.ctx`, `JSAtomicsWaiter.ctx`, auto-init property realm | native pointer or `JSContext *` | table indices | done (3p) |
| `JSAtomicsWaiter.async_rt` | `JSRuntime *`, tested for non-NULL and compared only | in-cage | 3a |
| `ctx->class_proto`, `rt->atom_array`, `rt->atom_hash` | arrays of in-cage pointers owned by trusted structures | in-cage | 3a |
| `JSObject` | `shape`, `prop`, `first_weak_ref`, GC links | in-cage | 3a |
| `JSObject.u.array` | `values` (fast arrays), `var_refs` | in-cage, unbounded by cage | 3a |
| `JSObject.u.func` | `function_bytecode`, `var_refs`, `home_object` | bytecode drives the interpreter's native stack frame | **3b** |
| `JSObject.u.cfunc.realm`, `JSFunctionBytecode.realm`, `JSWrappedFunction.realm` | context-table index; the interpreter switches realm only to a live context | done (3p) |
| internal `u.opaque` structs | `JSArrayBuffer`, `JSTypedArray`, `JSMapState`, `JSProxyData`, `JSPromiseData`, … | in-cage | 3a |
| `JSArrayBuffer.opaque` | host `void *` handed back to the realloc hook (`src/webgpu.c` held value, in-engine `NULL`) | tagged table entry | done |
| `JSCClosureRecord.opaque` | host `void *` | tagged table entry | done |
| `JSFunctionBytecode` | opcodes, `cpool`, `closure_var`, `vardefs`, `stack_size`, `var_count` | sizes and indexes the interpreter trusts for its native frame | **3b** |
| `JSAsyncFunctionState` / generators | a saved `JSStackFrame` with `cur_pc`, `arg_buf`, `var_buf` | native stack layout | **3b** |
| `JSShape` | `proto`, hash links, property atoms | in-cage | 3a |
| `JSString` ropes, atom table, `atom_array` | in-cage | 3a |
| typed array / buffer lengths | `byte_length`, `offset`, `length`, `count` | bounded by the 32 GB guard | done |
| embedder structs | `ns_node.js_wrapper`, listener records, `ns_js` | outside the cage pointing in; decoded only through the engine | fine |

After 3p the remaining rows that turn an in-cage write into native
code execution are the two bold `3b` rows, bytecode and saved frames;
everything else is data corruption inside the cage or a step on the way
to those two.

## Stage 3: a real boundary

Pointer compression turns every heap-to-heap reference into a 32-bit
offset from the cage base. QuickJS already carries a 64-bit value layout
with a 32-bit pointer field, the NaN-boxing mode used on 32-bit targets;
enabling it on 64-bit and decoding the pointer field as base plus offset
gives compressed values, and the manual work is converting the raw
object, shape, string and list-link pointers inside heap structures.

Scoping notes for 3a, from reading the tree after 3p:

- The value layout is fully encapsulated. Every pointer enters or leaves
  a `JSValue` through `JS_MKPTR` / `JS_VALUE_GET_PTR` and their typed
  wrappers (650 sites in the engine, 199 in the bindings), and no
  engine or binding code touches the struct's `.tag` or `.u` members
  directly. A `JS_CAGE_BOXING` mode in `quickjs.h`, the NaN-boxing
  encoding with the low 32 bits holding `pointer - cage_base`, is
  therefore a header change plus one exported cage base, and it halves
  every value to 8 bytes.
- It makes the cage mandatory in that build: a 32-bit offset cannot name
  memory outside the reservation, which is the point, so the runtime
  must refuse to start without one instead of falling back to the
  process heap. ASan and 32-bit builds keep today's layouts.
- Offset zero must never be handed out, so that a `NULL` pointer boxed
  into a value stays distinguishable; the allocator will keep the first
  chunk of the cage permanently reserved.
- Step two is the struct fields: 179 `JSObject *`, 75 `JSString *`,
  33 `JSShape *` and 29 `JSValue *` members, plus the GC list links,
  converted to 32-bit offsets behind typed accessors the way V8 did it
  field by field. Until that step the heap still holds raw 64-bit
  pointers next to compressed values, so step one is a memory and
  exploit-cost improvement, not yet the boundary.

### Stage 3a.1: caged values (done)

`JS_CAGE_BOXING`, on by default in 64-bit builds without
AddressSanitizer, makes `JSValue` an 8-byte word in the NaN-boxing
encoding whose low 32 bits hold `pointer - cage_base`. `JS_MKPTR` and
`JS_VALUE_GET_PTR` are the only two macros that changed; every property
slot, array element, stack slot, constant pool entry and argument now
names cage memory and nothing else, and all of them are half their
former size. The cage became the engine's own: `JS_NewRuntime()`
reserves it on first use, gives each runtime a heap inside it and
destroys that heap with the runtime, so `qjs`, `qjsc` and test262 run
the production layout. The exported `js_cage_pointer_base` is the one
global the decode reads.

Consequences worth knowing: the cage is 4 GB for the whole process,
shared by the page runtime, its workers and WebAssembly memory, the same
limit V8's pointer-compression cage has; a runtime cannot be created
without it; and `NULL` never appears inside a value, which the allocator
guarantees by never handing out offset zero.

### Stage 3a.2: compressed fields (plan)

The pointers that remain raw inside heap structures, with the number of
engine sites that read or write each, in the order they will be
converted to 32-bit cage offsets behind typed accessors. The GC list
links come first because a corrupted link is an unlink write outside the
cage, the same primitive the arena bypass removed.

| Field | Sites | Batch |
|---|---|---|
| `JSGCObjectHeader.link`, `JSTypedArray.link`, other `list_head` members in cage objects | 20 direct, plus the `list.h` macro uses over those lists | 1 |
| `JSObject.shape`, `JSObject.prop`, `JSShape.proto`, `JSShape.shape_hash_next` | 55, 43, 29, 9 | 2, **done** |
| `JSObject.u.array.u.values`, `u.array.u.var_refs`, `u.func.var_refs`, `u.func.home_object` | 41, 13, 11 | 3, **done** |
| `JSFunctionBytecode.cpool`, `byte_code_buf`, `closure_var`, `vardefs`, `pc2line_buf` | 17, 22, 16, 29, 10 | 4, **done** |
| `JSStringSlice.parent`, `first_weak_ref` on objects and strings, `JSWeakRefRecord.next_weak_ref` | 4, 10, 11 | 5, **done** (rope children are boxed values already) |
| internal class structs reached from `u.opaque` (`JSArrayBuffer`, `JSTypedArray`, `JSMapState`, …) and `u.func.function_bytecode` | 22 union members, about 200 sites | 6, **done** |

Each batch is a mechanical rewrite in the style of stage 2a: the field
is declared `JS_CPTR(T)`, which is `uint32_t` under cage boxing and `T`
otherwise, reads go through `js_cload(T, field)` and writes through
`js_cstore(field, ptr)`, with offset zero meaning `NULL` (the allocator
never hands out offset zero). Each batch is benchmarked with sitebench
before it merges.

Batches 2 and 3 are done: every `JSObject` now holds its shape, property
array, fast-array values, mapped-arguments and closure variable arrays
and home object as 32-bit offsets, and every `JSShape` its prototype and
hash-chain link. The two engine paths that took the address of the
shape field now work on a local and store it back; the shape hash unlink
walks the chain with explicit previous and current pointers instead of a
pointer to pointer. The allocation stress script runs in 0.33 s and
176 MB, down from 179 MB; `qjs -d` reports `JSObject` at 72 bytes.

Batch 4 is done too: the five arrays a `JSFunctionBytecode` carries as
self pointers into its own allocation (the opcodes, the constant pool,
the closure-variable and variable-definition tables and the line table)
are offsets, so the interpreter's `pc` and every constant-pool read start
from cage memory even if the function object was corrupted. The sizes
and indexes those arrays are read with are still trusted from in-cage
data; that is what stage 3b changes.

Batch 5 is done: the weak-reference chain that hangs off every object
and symbol (`first_weak_ref` and each record's `next_weak_ref`) and the
parent pointer of a string slice are offsets. The chain is manipulated
through pointers to slots, so the helpers that returned or took a
`JSWeakRefRecord **` now return or take a pointer to the compressed slot
and decode on read. Rope children were already boxed values.

Batch 6 is done: every member of the object's opaque union is an offset,
which covers the structures behind ArrayBuffer, typed arrays and
DataView, Map and Set, Proxy, bound functions, promises and their
resolving functions, generators, every iterator kind, async functions
and the C-function and closure records, plus the function object's
pointer to its bytecode. Host classes keep their external-pointer-table
index in the same slot, and the ShadowRealm object keeps its context as
a context-table index. With this, the only raw pointers left inside
cage objects are the GC list links (batch 1), the typed-array back
pointers inside `JSTypedArray` (`obj`, `buffer`) and the internal
structures' own members, which later batches take as they come up. The
allocation stress script is at 0.37 s and 174 MB.

Trusted bytecode space allocates `JSFunctionBytecode`, its opcode buffer,
closure tables and the saved frames of generators and async functions
from memory outside the cage, reached through an indirect table, so the
interpreter sizes and indexes its native stack frame from data an
in-cage attacker cannot reach.

These two stages are the ones that diverge the fork from upstream for
good, which is why they come last and only after the earlier stages have
been measured.

### Stage 3p: trusted runtime structures (done)

Shared preparation for both halves, done first because it closed the
shortest path from an in-cage write to native code: `JSRuntime` held
the allocator hooks and the class table with every finalizer, mark and
call hook, and heap objects held raw realm pointers that the
interpreter switched to on every call, so a forged realm gave a forged
runtime gave a code pointer in three dependent reads.

- `JSRuntime`, every `JSContext` and the runtime's `class_array` are
  allocated from trusted memory (plain libc, outside the reservation)
  through a trusted block path in the fork's allocator front end: the
  block keeps the 8-byte header the GC's reference count lives in,
  carries its size in the 8 bytes before that, and is recognised by a
  reserved size-class index, so `js_free_rt`, `js_realloc_rt` and the
  memory accounting keep working on it unchanged.
- A trusted **context table** (65 536 slots, outside the cage) hands
  every context an index at creation. The nine heap-resident `JSContext`
  fields now hold that index instead of a pointer: C-function realm,
  bytecode realm, wrapped-function realm, job-queue entries,
  finalization-registry records and entries, async Atomics waiters and
  auto-init property records. `js_ctx_get()` masks the index and reads
  the slot, so a forged realm selects a live context or `NULL`, never
  memory of the attacker's choosing.
- The two native function pointers still in heap structures after
  stage 2b, the job-queue function and the C-module init function, are
  interned in the code pointer table.

Realm fields are still reference counted through `JS_DupContext` /
`JS_FreeContext` on the decoded pointer. The embedder maps runtime to
cage heap through a small table of its own now that the runtime no
longer lives in the cage (`src/js_cage.c`).

## Benchmarks

Sitebench comparisons, old build against new, per stage. Runs here used
`RUNS=2 VISUAL_RUNS=1` on a 4-core container, so single-site deltas
under about 10 % are noise; the aggregate lines are what to read.

Stage 1a, `scripts/sitebench/ab.sh` on google, anthropic, bbc, cnn,
guardian (2025-11-09):

| Build | Parity | First paint ÷ Chrome FCP | Main CPU ÷ Chrome | Memory ÷ Chrome |
|---|---|---|---|---|
| before (process heap) | 84.9 | 1.46× | 3.71× | 0.26× |
| after (cage) | 84.6 | 1.37× | 3.84× | 0.27× |

| Site | CPU before | CPU after | RSS before | RSS after |
|---|---|---|---|---|
| google | 239 ms | 249 ms | 76.5 MB | 76.9 MB |
| anthropic | 710 ms | 668 ms | 138.5 MB | 138.9 MB |
| bbc | 3037 ms | 2893 ms | 452.0 MB | 454.0 MB |
| cnn | 6766 ms | 7193 ms | 667.6 MB | 681.7 MB |
| guardian | 5111 ms | 5061 ms | 379.0 MB | 380.8 MB |

Memory grows by 0.5 to 2 % from slab rounding and the out-of-cage
metadata; CPU is within noise in both directions.

Stage 2a, same sites, before = stage 1a build, after = masked buffer
pointers (2025-11-09):

| Build | Parity | First paint ÷ Chrome FCP | Main CPU ÷ Chrome | Memory ÷ Chrome |
|---|---|---|---|---|
| before (stage 1a) | 87.1 | 1.41× | 3.40× | 0.27× |
| after (stage 2a) | 86.9 | 1.24× | 3.52× | 0.27× |

| Site | CPU before | CPU after | RSS before | RSS after |
|---|---|---|---|---|
| google | 279 ms | 264 ms | 77.0 MB | 77.9 MB |
| anthropic | 727 ms | 697 ms | 138.8 MB | 139.2 MB |
| bbc | 3574 ms | 3094 ms | 454.0 MB | 454.1 MB |
| cnn | 7509 ms | 7170 ms | 683.1 MB | 681.8 MB |
| guardian | 5300 ms | 5299 ms | 381.0 MB | 381.7 MB |

The masking adds an AND and an OR per buffer access; nothing in these
loads shows it above the noise.

Stage 2b, pointer tables only (arena still active), before = stage 2a
build (2025-11-09):

| Build | Parity | First paint ÷ Chrome FCP | Main CPU ÷ Chrome | Memory ÷ Chrome |
|---|---|---|---|---|
| before (stage 2a) | 87.2 | 1.47× | 3.36× | 0.27× |
| after (stage 2b tables) | 86.8 | 1.33× | 3.38× | 0.27× |

| Site | CPU before | CPU after | RSS before | RSS after |
|---|---|---|---|---|
| google | 240 ms | 247 ms | 76.9 MB | 77.9 MB |
| anthropic | 689 ms | 711 ms | 139.2 MB | 139.6 MB |
| bbc | 3153 ms | 3048 ms | 453.2 MB | 452.5 MB |
| cnn | 6960 ms | 7133 ms | 682.9 MB | 682.8 MB |
| guardian | 5114 ms | 5070 ms | 382.2 MB | 381.9 MB |

The table lookups are a masked two-level array read per host-object
access and per native call; within noise on every site.

Bypassing the fork's arena allocator while the cage is active costs
more on the allocation stress script: 0.36 s and 183 MB against 0.32 s
and 177 MB with the arena, so about 10 % more time on an allocation-bound
microbenchmark and 3 % more memory from 16-byte size-class rounding of
the 8-byte block header.

Stage 2b complete (tables and arena bypass), before = stage 2a build
(2025-11-09):

| Build | Parity | First paint ÷ Chrome FCP | Main CPU ÷ Chrome | Memory ÷ Chrome |
|---|---|---|---|---|
| before (stage 2a) | 83.0 | 1.58× | 3.62× | 0.26× |
| after (stage 2b complete) | 82.5 | 1.47× | 3.63× | 0.26× |

| Site | CPU before | CPU after | RSS before | RSS after |
|---|---|---|---|---|
| google | 295 ms | 259 ms | 78.4 MB | 78.1 MB |
| anthropic | 779 ms | 739 ms | 139.6 MB | 139.9 MB |
| bbc | 3024 ms | 3690 ms | 454.3 MB | 450.7 MB |
| cnn | 7448 ms | 7531 ms | 682.0 MB | 683.1 MB |
| guardian | 5036 ms | 5014 ms | 382.1 MB | 382.8 MB |

The aggregate is flat. The bbc outlier is run-to-run noise on that
site, whose CPU has ranged from 2.9 s to 3.7 s across the eight captures
in this document with no consistent direction; the allocation-bound
microbenchmark above is the honest measure of the arena bypass, about
10 % on pure allocation churn and invisible in page loads.

Stage 3p, before = stage 2b complete (2025-11-09):

| Build | Parity | First paint ÷ Chrome FCP | Main CPU ÷ Chrome | Memory ÷ Chrome |
|---|---|---|---|---|
| before (stage 2b) | 85.6 | 1.60× | 3.99× | 0.29× |
| after (stage 3p) | 85.8 | 1.18× | 3.98× | 0.30× |

| Site | CPU before | CPU after | RSS before | RSS after |
|---|---|---|---|---|
| google | 239 ms | 234 ms | 77.5 MB | 79.2 MB |
| anthropic | 657 ms | 677 ms | 139.6 MB | 139.4 MB |
| bbc | 3136 ms | 3163 ms | 454.6 MB | 452.5 MB |
| cnn | 7521 ms | 7086 ms | 682.0 MB | 681.7 MB |
| guardian | 5161 ms | 5151 ms | 382.4 MB | 382.4 MB |

A context-table read replaces a pointer load on every realm switch;
flat on every site. The allocation stress script is unchanged at
0.32 to 0.39 s and 183 MB.

Stage 3a.1, caged values, before = stage 3p (2025-11-09):

| Build | Parity | First paint ÷ Chrome FCP | Main CPU ÷ Chrome | Memory ÷ Chrome |
|---|---|---|---|---|
| before (stage 3p) | 86.1 | 1.55× | 3.87× | 0.30× |
| after (stage 3a.1) | 85.6 | 1.41× | 3.97× | 0.30× |

| Site | CPU before | CPU after | RSS before | RSS after |
|---|---|---|---|---|
| google | 242 ms | 240 ms | 77.4 MB | 78.4 MB |
| anthropic | 680 ms | 723 ms | 139.1 MB | 138.9 MB |
| bbc | 3019 ms | 3030 ms | 452.9 MB | 451.3 MB |
| cnn | 7077 ms | 7331 ms | 682.2 MB | 681.5 MB |
| guardian | 5019 ms | 5026 ms | 382.4 MB | 382.4 MB |

This is the first stage with a visible cost: about 2 to 3 % more main
thread CPU in aggregate, concentrated on the script-heavy sites, from
the base-plus-offset decode on every value that holds a pointer. The
allocation stress script is unchanged in time (0.35 s) and 2 % smaller in
peak memory (179 MB against 183 MB) from the halved values. Process RSS
on real pages is dominated by images, layout and network buffers, so the
value shrink does not show there. A follow-up worth trying is caching
the cage base in a local inside the interpreter loop, where most decodes
happen.

Stage 3a.2 batches 2 and 3, before = stage 3a.1 (2025-11-09):

| Build | Parity | First paint ÷ Chrome FCP | Main CPU ÷ Chrome | Memory ÷ Chrome |
|---|---|---|---|---|
| before (stage 3a.1) | 87.2 | 1.61× | 3.54× | 0.26× |
| after (batches 2 and 3) | 86.9 | 1.53× | 3.55× | 0.25× |

| Site | CPU before | CPU after | RSS before | RSS after |
|---|---|---|---|---|
| google | 247 ms | 249 ms | 77.7 MB | 78.2 MB |
| anthropic | 704 ms | 694 ms | 139.6 MB | 138.9 MB |
| bbc | 3064 ms | 3073 ms | 452.5 MB | 453.0 MB |
| cnn | 7797 ms | 7973 ms | 681.7 MB | 679.8 MB |
| guardian | 5141 ms | 5248 ms | 382.9 MB | 382.3 MB |

Flat in aggregate; the shape and property-array decodes sit on the
property access path and cost nothing measurable beyond the value
decode already paid for in 3a.1.

Stage 3a.2 batch 4, bytecode arrays, before = batches 2 and 3
(2025-11-09):

| Build | Parity | First paint ÷ Chrome FCP | Main CPU ÷ Chrome | Memory ÷ Chrome |
|---|---|---|---|---|
| before (batches 2 and 3) | 87.0 | 1.71× | 3.52× | 0.27× |
| after (batch 4) | 87.0 | 1.41× | 3.47× | 0.27× |

| Site | CPU before | CPU after | RSS before | RSS after |
|---|---|---|---|---|
| google | 265 ms | 279 ms | 77.5 MB | 78.3 MB |
| anthropic | 736 ms | 688 ms | 138.9 MB | 139.4 MB |
| bbc | 3098 ms | 3040 ms | 452.2 MB | 451.3 MB |
| cnn | 7399 ms | 7351 ms | 681.8 MB | 682.0 MB |
| guardian | 5143 ms | 5102 ms | 383.1 MB | 383.6 MB |

Flat; the bytecode-array decodes happen once per call, not per
instruction.

Stage 3a.2 batch 5, weak references and slices, before = batch 4
(2025-11-09):

| Build | Parity | First paint ÷ Chrome FCP | Main CPU ÷ Chrome | Memory ÷ Chrome |
|---|---|---|---|---|
| before (batch 4) | 83.4 | 1.46× | 3.43× | 0.26× |
| after (batch 5) | 83.4 | 1.36× | 3.34× | 0.26× |

| Site | CPU before | CPU after | RSS before | RSS after |
|---|---|---|---|---|
| google | 254 ms | 238 ms | 78.4 MB | 77.7 MB |
| anthropic | 686 ms | 707 ms | 139.4 MB | 138.7 MB |
| bbc | 3088 ms | 3012 ms | 451.8 MB | 451.8 MB |
| cnn | 7325 ms | 7329 ms | 680.9 MB | 679.9 MB |
| guardian | 5058 ms | 4993 ms | 383.0 MB | 382.5 MB |

Flat.
