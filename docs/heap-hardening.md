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
| 3a | Pointer compression (8-byte values, 32-bit cage offsets) | planned |
| 3b | Trusted space for bytecode and saved frames | planned |

Stage 1b, moving cookies, HSTS, the HTTP cache and storage behind the
shell so a compromised renderer holds no credentials, is independent of
the heap stages and remains the complementary architectural step that
`docs/tab-isolation.md` already lists. It limits what a compromise is
worth; the stages here limit how a bug becomes a compromise.

## Stage 1a: the cage

`src/js_cage.c` reserves one large range of virtual address space per
renderer process at the first runtime creation and serves every QuickJS
allocation from it. It plugs in through the engine's public
`JS_NewRuntime2` allocator hook, so the cage itself needs **no engine
source changes**. The fork's own small-block arena allocator
(`src/quickjs/quickjs.c`) steps aside while a cage is active: its arena
headers, free lists and list links live inside the blocks it manages,
so a corrupted heap could have steered `js_arena_free` into an unlink
write outside the cage. With the cage on, every allocation goes to the
cage allocator and its out-of-cage bookkeeping; without one, the arena
works as before.

Layout:

- The reservation is 64 GB when the address space allows it, halving down
  to 1 GB otherwise, aligned to its own size so a future offset mask can
  be a plain bit-and. Inaccessible guard regions of 8 GB sit on both
  sides. Nothing is committed until used.
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

Switches: `NS_JS_HEAP_CAGE=0` in the environment falls back to the
process heap, for debugging. Builds with AddressSanitizer and 32-bit
builds use the process heap unconditionally. The cage prints its base
and size under `G_MESSAGES_DEBUG=all`.

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
