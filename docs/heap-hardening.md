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
| 2b | Tagged external pointer table and code pointer table | planned |
| 2c | Inventory of raw host and function pointers left in the heap | planned |
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
`JS_NewRuntime2` allocator hook, so **no engine source changes**; the
fork's small-block arena allocator (`src/quickjs/quickjs.c`) keeps
working unchanged and simply draws its 4 KB arenas from the cage.

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

Stage 2b replaces the raw pointer behind `JS_SetOpaque` / `JS_GetOpaque`
with an index into a table outside the cage whose entries carry the
class id as a type tag, and interns every registered C function pointer
in a code pointer table so a heap object holds an index rather than a
code address. Both are implemented inside the fork behind the existing
API, so the call sites in `src/js.c`, `src/webgpu.c`, `src/wasm.c` and
the canvas code do not change.

Stage 2c keeps an inventory, in this document, of every raw function or
host pointer still resident in heap objects after 2a and 2b, with the
plan for each.

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
