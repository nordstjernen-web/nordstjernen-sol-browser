# WebAssembly

Nordstjernen ships a **complete WebAssembly JS API** backed by a vendored
subset of [WAMR](https://github.com/bytecodealliance/wasm-micro-runtime)
(wasm-micro-runtime, Apache-2.0) — its fast interpreter, with
**reference types** and **bulk memory** enabled. There is no JIT and no
AOT: wasm executes in a portable interpreter, the same on Linux, macOS,
Windows and Android.

This is enough to run real-world **wasm-bindgen** bundles (Rust →
`wasm32-unknown-unknown`), including externref-heavy ones. The flagship
test case is Discord's `libdiscore`, which signs all of Discord's API
traffic and hard-gates their login page on WebAssembly.

## What is implemented

The `WebAssembly` namespace (`src/wasm.c`):

| Surface | Notes |
|---------|-------|
| `WebAssembly.validate(bytes)` | full load + validation |
| `WebAssembly.compile(bytes)` / `new Module(bytes)` | returns a reusable `Module` |
| `WebAssembly.instantiate(bytes\|module, imports)` | resolves to `{module, instance}` / `Instance` |
| `WebAssembly.instantiateStreaming` / `compileStreaming` | accepts a `Response` or a promise of one |
| `Instance.exports` | functions, memories, tables as own enumerable properties |
| `Memory.buffer` | live `ArrayBuffer` over linear memory; detached on every grow (from JS *or* from wasm `memory.grow`) |
| `Memory.grow(pages)` | returns the old page count |
| `Table.get/set/grow/length` | funcref tables hand back callable wrappers; externref tables round-trip arbitrary JS values |
| `new Memory({initial, maximum})` | standalone memory backed by staging bytes; adopted by the instance when passed as a memory import |
| `new Table({element})` / `new Global({value, mutable}, v)` | standalone constructors; a `Global` holds `i32`/`i64`/`f32`/`f64`/`externref`/`funcref` with a mutable `value` accessor and `valueOf` |
| Imported memories/tables/globals | a provided `Memory`'s staging bytes seed the instance memory (data segments win); a provided `Global`'s (or plain number/BigInt) current value initializes the imported global at instantiate time |
| `CompileError`, `LinkError`, `RuntimeError` | thrown from the matching phases; `LinkError` lists every unresolved import by name |

Marshalling at the JS ↔ wasm boundary:

- `i32`/`f32`/`f64` ↔ numbers, `i64` ↔ `BigInt`.
- `externref` ↔ any JS value. Values are boxed and registered with
  WAMR's externref table; WAMR's reclaim pass frees boxes that wasm no
  longer holds, with a cleanup callback dropping the JS reference.
- Imports are plain JS functions, called with marshalled arguments.
  A JS exception thrown inside an import aborts the wasm frames and
  re-emerges as the same JS exception at the outermost `exports.*`
  call — the pattern wasm-bindgen's `__wbindgen_throw` relies on.
- Multi-value results come back as an array.

## What is not implemented

- **Live mutation of imported globals** — a `Global` passed as an
  import is *snapshotted* at instantiate time; later `.value`
  assignments do not propagate into the running instance (and wasm
  writes to a mutable imported global are not visible on the JS
  object).
- **Ref-typed (`externref`/`funcref`) imported globals** — standalone
  `Global`s of these types work from JS, but linking one as a module
  import is skipped.
- **Threads/atomics, GC, exception handling, memory64** — off in
  the WAMR build. Pages that feature-detect get a clean
  `CompileError` and fall back.

## SIMD

`v128` and the whole SIMD opcode set are supported (`WebAssembly.validate`
on a SIMD module returns `true`) when the build has the header-only
[SIMDe](https://github.com/simd-everywhere/simde) library, which WAMR's
fast interpreter uses to execute each `v128` operation as one native SSE/
NEON operation instead of eight to sixteen scalar ones. `meson` takes
the system `simde` package (Debian/Ubuntu `libsimde-dev`, Fedora
`simde-devel`, Homebrew `simde`) or else fetches the pinned release
through `subprojects/simde.wrap` (never committed); `-Dwasm_simd=disabled`
drops it, and the configure summary shows `WebAssembly SIMD (SIMDe)`.
Sites that ship a SIMD and a scalar build of the same module — chess
engines, codecs, image processing — then pick the SIMD build, which under
an interpreter is several times faster than the scalar one.

## Vendored runtime

`src/wamr/` holds the WAMR subset (interpreter, loader, common runtime,
platform layers for Linux/macOS/Windows), built by a hand-written
`meson.build` like the other in-tree libraries (lexbor, QuickJS,
Wuffs). It is a fork we patch freely; local changes so far:

- `wasm_func_type_get_result_valkind` returned garbage for externref
  results, which broke native-symbol signature checks for every
  wasm-bindgen import returning externref.
- Raw native dispatch copied the wrong pointer for externref
  arguments (`argv_src` instead of the converted object).
- An **enlarge-memory success callback**
  (`wasm_runtime_set_enlarge_mem_success_callback`) so the JS
  `Memory.buffer` can be detached the moment linear memory moves —
  without it, wasm-bindgen's cached `Uint8Array` views would read
  freed memory after a wasm-side `memory.grow`.
- Winsock init dropped from the Windows platform layer (no sockets
  are used) and the wasm-c-api coupling stubbed out.
- `ns_wamr.{c,h}` — narrow accessors into runtime internals: table
  element access by export name (externref `get`/`set`/`grow`) and an
  externref reclaim trigger, called from the JS boundary between
  top-level calls.

Imports resolve through WAMR's global native-symbol registry: each
`instantiate` registers the page's import functions as raw natives,
resolves the module, and immediately unregisters them. The
`NativeSymbol` blocks (and the JS functions they bind) live as long as
the `Module` object.

## Debugging

`ND_WASM_LOG=1` switches WAMR to verbose logging (load/link/runtime
traces, including the exact signature string a failed import was
checked against).

```sh
ND_WASM_LOG=1 ./builddir/src/gtk/nordstjernen --headless --dump=text \
    --url=https://discord.com/login --settle-ms=20000
```

## Security posture

wasm runs in the same process as the page's JS, interpreted, with
software bounds checks on every memory access (the WAMR build sets
`WASM_DISABLE_HW_BOUND_CHECK=1` and `WASM_DISABLE_STACK_HW_BOUND_CHECK=1`
— no guard-page tricks, no signal handlers). A wasm module
can reach exactly what its JS imports hand it: there are no WASI
syscalls, no filesystem, no sockets. The interpreter's operand stack is
capped (1 MB) and traps surface as catchable `RuntimeError`s.
