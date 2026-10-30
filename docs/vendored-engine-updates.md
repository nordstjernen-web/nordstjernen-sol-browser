# Vendored engine updates — QuickJS & Lexbor

Nordstjernen carries in-tree forks of two upstream C libraries that we
modify freely for browser integration:

- **QuickJS** (`src/quickjs/`) — forked from
  [quickjs-ng](https://github.com/quickjs-ng/quickjs).
- **Lexbor** (`src/lexbor/`, sources under `src/lexbor/source/lexbor/`) —
  forked from [lexbor](https://github.com/lexbor/lexbor), trimmed to the
  modules we use (`core dom encoding html ns ports punycode tag unicode url
  utils`); upstream's `css`, `engine`, `selectors` and `style` modules and
  its test and `utils/` tooling are not carried.

We track each upstream's development branch (`master`; some older entries
below call it `main`). This document records each refresh:
the upstream point we rebased onto, the upstream changes that landed, and
the local modifications that were preserved on top.

## How an update is done

For each fork, the true fork-base commit is recovered (the upstream commit
whose tree equals ours minus our deliberate edits), then every carried file
is classified:

- **copy** — we never touched it, upstream did → take upstream's version.
- **keep** — only we touched it (upstream unchanged in range) → keep ours.
- **merge** — both touched it → 3-way merge (`git merge-file ours BASE
  theirs`), reviewing every hunk.

Sources are listed explicitly in each `meson.build` (`src/quickjs/meson.build`,
`src/lexbor/meson.build`), so new upstream files in unused areas are simply
not compiled; the build is the final gate. After a QuickJS refresh, also build
`-Dquickjs=quickjs` and fix the adapter if the binding changed (see
[`quickjs.md`](quickjs.md)).

## 2026-10-30 — QuickJS 270b8c9 → 7b5f588, Lexbor f4cbbcd → 145872a

### QuickJS

| | value |
|---|---|
| Fork base | `270b8c9` |
| Updated to | `7b5f588` (master, "Use $RUNNER_TEMP instead of /tmp in CI recipes (#1861)"), less the commits listed as not taken |
| Reported version | `0.17.0` (unchanged) |

Nine upstream commits; three are taken, done as a three-way merge per file
(base `270b8c9`).

- **Memory safety** — freeing a `WeakMap` whose values are symbols that are
  also keys of the same map could free records the finalizer had yet to
  visit; the finalizer now drops every weak reference before freeing any
  value (`25a005b`, #1844). `JS_ReadBigInt` leaked its partly read BigInt on
  truncated input (`7a23d8e`).
- **Language** — `Array.fromAsync` calls the iterator method and the mapping
  function through an internal call instead of looking up `.call`, so a
  patched `Function.prototype.call` no longer changes it (`c1ed194`, #1795).
  `builtin-array-fromasync.h` is regenerated with the fork's own `qjsc`
  (`qjsc -C -ss`), since the fork's `BC_VERSION` differs from upstream's.

**Not taken.**

- Asynchronous dynamic `import()` (`d4cd8f4`) conflicts with the fork's
  import-phase plumbing in `js_dynamic_import_job` and `JS_LoadModule`
  (`import defer`, `import source`) and changes the public `JS_LoadModule`
  signature. It is an opt-in embedder API the browser does not use.
- The Cosmopolitan libc build, CI and `os.platform` detection (`11e08c0`,
  `231400a`, `cea851c`), upstream's CI runner change (`7b5f588`) and the WASI
  reactor function-table export (`e680786`, a `CMakeLists.txt` block the fork
  does not carry). Upstream's `.github/workflows/` files stay at `270b8c9`.
- The new tests `tests/bug1795.js` and `tests/bug1835.js` and `api-test.c`
  are not carried; `tests/test_bjson.js` takes upstream's truncated-BigInt
  case.

**Local modifications preserved.** `quickjs.c` merged without a conflict;
every browser hook carries over unchanged. No API changed, so the binding and
`-Dquickjs=quickjs` adapter are untouched, and the bytecode format is
unchanged.

### Lexbor

| | value |
|---|---|
| Fork base | `f4cbbcd` |
| Updated to | `145872a` (master, "HTML: grow tokenizer temporary buffers geometrically (#433)") |
| Version | `3.1.0` (unchanged) |

Six upstream commits; three touch the modules we carry (the three
Selectors fixes, `4c44b8c`, `6ce5125` and `f6c1d06`, are in a module this
fork does not build).

- **HTML** — the `textarea` start tag switches to the "text" insertion mode
  again after dropping a leading newline, so its contents are no longer
  parsed by the "in body" rules, which recreated a stale formatting element
  inside it: `<div><a></div><textarea>ab</textarea>x` gave
  `<textarea><a>ab</a></textarea>` (`0c657de`, #434).
- **HTML** — the tree builder caches, per entry of the stack of open
  elements, whether a `p` element is in button scope, so a start tag such as
  `div` no longer walks the whole stack and N nested elements parse in linear
  time instead of quadratic (`730b667`, #437).
- **Tokenizer** — `lxb_html_tokenizer_temp_realloc()` grows its buffer
  geometrically instead of by 4 KB each time, checks the new size for
  overflow, and keeps the old buffer when `realloc` fails instead of losing
  it (`145872a`, #433).

All six files (`html/tokenizer.h`, `html/tree.c`, `html/tree.h`,
`html/tree/open_elements.c`, `html/tree/open_elements.h`,
`html/tree/insertion_mode/in_body.c`) carry no local modification, so
upstream's versions are copied. The new tests are not carried.

The memory-safety fixes upstream made since the previous fork bases are all
in the fork already: `lexbor_mraw_realloc()` (`f3f6fa7`),
`lxb_dom_character_data_replace()` (`cf31487`, #408) and the encoder and
decoder bounds checks (`47be1dc`, `5d3a6f5`) since `327a8b6`, and the
`lexbor_array_obj_t` sizes (#428) since `f4cbbcd`.

### Verification

- `meson compile -C builddir` (MSYS2 MINGW64, GCC) builds and links with no
  warnings.
- `./scripts/dev.sh smoke` passes.
- A headless page gives the expected results: 50 rounds of `WeakMap`s
  holding symbol keys and symbol values, some values also keys, are freed
  without a fault; `Array.fromAsync` with a mapping function; the two
  textarea cases above and a foster-parented textarea; a `p` under 400
  nested `div`s closed by a following `div`; a `div` after a `button` inside
  a `p` staying inside the button; the adoption agency case
  `<p>1<b>2<p>3<i>4</b>5<div>6`; and a 200,000-character attribute value.

## 2026-10-22 — QuickJS 5271a40 → 270b8c9

| | value |
|---|---|
| Fork base | `5271a40` |
| Updated to | `270b8c9` (master, "Export all API symbols in the QJS_WASI_REACTOR build (#1804)") |
| Reported version | `0.17.0` (unchanged) |

Two upstream commits.

- **Unicode** — `unicode_script()` returned through its own `fail` label
  after freeing its ranges on an allocation failure, looping forever
  (`081d50c`). `libunicode.c` is a three-way merge; the hunk applied cleanly.
- **WASI reactor** — the `qjs_wasi` target now defines `BUILDING_QJS_SHARED`
  so every API symbol is exported (`270b8c9`). The fork's `CMakeLists.txt`
  carries no WASI reactor block and no `qjs-wasi-reactor.c`, so the hunk is
  dropped. Upstream's CI workflow is copied unmodified.

## 2026-10-18 — Lexbor 327a8b6 → f4cbbcd

| | value |
|---|---|
| Fork base | `327a8b6` |
| Updated to | `f4cbbcd` (master, "Core: fixed allocation and move sizes in lexbor_array_obj_t. (#428)") |
| Version | `3.1.0` (unchanged) |

One upstream commit. `lexbor_array_obj_init()` and `lexbor_array_obj_expand()`
multiplied the bytes they need by `sizeof(uint8_t *)`, so every object array
the HTML tokenizer, tree builder and parse-error log keep was eight times
larger than needed on 64-bit hosts, and `lexbor_array_obj_delete()` moved
eight times the bytes of the remaining entries, staying in bounds only
because of that oversized allocation. `core/array_obj.c` carries no local
modification, so upstream's file is copied. Its new tests are not carried.

## 2026-10-18 — QuickJS 9b06171 → 5271a40

| | value |
|---|---|
| Fork base | `9b06171` |
| Updated to | `5271a40` (master, "Upgrade macos-14 runners to macos-15 (#1830)") |
| Reported version | `0.17.0` (unchanged) |

One upstream commit. It moves upstream's macOS CI jobs to `macos-15` runners
and touches only `.github/workflows/ci.yml`, which `src/quickjs/` carries
unmodified and never runs, so that file is copied and no engine source
changes. Every other carried file is identical to `5271a40` apart from the
fork's own modifications.

## 2026-10-15 — QuickJS 60984dc → 9b06171

| | value |
|---|---|
| Fork base | `60984dc` |
| Updated to | `9b06171` (master, "Explain new security policy in SECURITY.md (#1818)") |
| Reported version | `0.17.0` (unchanged) |

Five upstream commits, done as a three-way merge per file (base `60984dc`).

- **Language** — an untagged template literal converts each substitution
  with `ToString` right after evaluating it, instead of calling
  `String.prototype.concat` on the first chunk (`c359cac`). The `concat`
  predefined atom is gone, and `BC_VERSION` goes from 28 to 29. The
  precompiled builtins (`builtin-*.h`, `gen/*.c`) are upstream's
  regenerated blobs.
- **Errors** — `InternalError`, including out-of-memory, can no longer be
  caught by JS code (`90aa2c4`, `db0fe52`).

**Local modifications preserved.** One hunk conflicted, the `BC_VERSION`
definitions, where the fork keeps reading older stored data.

**Adaptation required.**

- `JS_ReadObject` accepts data written as version 27 or 28 and shifts the
  predefined atom indices past the removed `concat` atom, so IndexedDB
  records stored by an earlier build stay readable. Bytecode of those
  versions is refused; `NS_BYTECODE_CACHE_FORMAT_VERSION` is bumped so cached
  scripts are compiled again.
- The browser threw `InternalError` for failures a page should be able to
  catch: WebGPU object creation (`src/webgpu.c`), WebAssembly table and
  import failures (`src/wasm.c`, now `RangeError` or
  `WebAssembly.RuntimeError`), `crypto.getRandomValues` and the zlib
  streams (`src/js.c`). They now throw catchable `TypeError`/`RangeError`.

### Verification

- The GCC build compiles with no new warnings; a headless page using
  template literals renders.
- test262 main @ `d53ebc8b`: 0/82248 failures (the two fixes below cover
  the five failures that this test262 revision added).

## 2026-10-08 — QuickJS 0.16.2 → 60984dc

| | value |
|---|---|
| Fork base | `1ab8676` (tag `v0.16.2`) |
| Updated to | `60984dc` (master, "Tweak new object GC heuristic (#1806)"), sixteen commits past `v0.17.0` |
| Reported version | `0.16.2` → `0.17.0` (`QJS_VERSION_*` in `quickjs.h`) |

Forty-eight upstream commits, done as a three-way merge of the fork onto
upstream master (base `v0.16.2`).

- **Memory safety** — a use-after-free in `AsyncDisposableStack` when a
  promise's reject handler takes no argument (`05b2db9`); a reference-count
  bug in `Promise.withResolvers` (`49131a6`); an out-of-bounds read in
  `JS_NewTypedArray` (`3f57bfb`); undefined behaviour in
  `String.prototype.normalize` (`2c620e4`); `JS_FreeCStringUTF16` on slice
  strings (`396e1e0`); a use-after-free when `JS_NewContext` runs out of
  memory (`8e14a39`); a leak in `JSON.stringify` when an array index cannot
  be converted (`293ba08`).
- **Language** — `ToPropertyKey` of `obj[key] = value` happens when the value
  is stored, after the right-hand side, following the specification change
  (`0db3949`); `Reflect.set` with an undefined or null receiver returns
  `false` instead of throwing (`970500f`); a bound function takes its
  target's prototype (`6a9b531`); `Date.parse` reads Unicode spaces, such
  as the U+202F that locale formatting puts before AM/PM, as spaces
  (`140b26d`); `"".repeat(n)` no longer fails for a large `n` (`19dbe85`);
  `Iterator.from` wraps the iterator an iterable returns, so a string's or a
  plain iterable's iterator gains the helper methods, and reads its `next`
  once (`7761502`); constructing a BigInt typed array from a Number one, or
  the reverse, throws even when the source is empty (`5c87424`);
  `Object.preventExtensions` on a typed array over a resizable buffer throws,
  and `TypedArray.prototype.at` follows a resized buffer (`9290824`,
  `3a50dc5`); `import()` records its source position (`a6b82a3`).
- **Speed** — property keys are hashed before indexing a shape's table, so
  power-of-two integer keys no longer collide (`cad311e`); a bigger initial
  atom table (`4355069`); a faster `JS_DeleteGlobalVar` (`02368b6`); a
  peephole pass folds a negation into the following conditional jump
  (`03f4950`); `JSValue` writes all 64 bits of its payload, which GCC 14+
  needs to keep the interpreter fast (`2f0aa72`); a new-object GC heuristic
  (`60984dc`).
- **API** — the interrupt handler receives the `JSContext` that is running
  instead of the runtime (`876442b`); `JSClass` is private to `quickjs.c`
  (`5e8c99e`); `JS_NewPrivateSymbol`, `JS_GetPropertyValue`,
  `JS_SetPropertyValue` and `JS_CGETSET_MAGIC_DEF2` are public (`7e322b3`,
  `9963110`).
- **Serialization** — `JS_WriteObject` encodes predefined atoms in one or a
  few bytes, and `BC_VERSION` goes from 27 to 28 (`5301314`). The precompiled
  builtins (`builtin-*.h`, `gen/*.c`) are upstream's regenerated blobs.

**Local modifications preserved.** `quickjs.h`, `libregexp.c`, `cutils.h`
and `quickjs-opcode.h` carry the same local delta against upstream as before,
line for line. In `quickjs.c` eight hunks conflicted; every browser hook
carried over unchanged.

**Overlapping fixes.** Upstream fixed three things the fork had already
fixed locally:

- `with` binding lookups. Upstream's `a2ab90a` re-probes the binding object
  in `with_get_var` and `with_get_ref`, as our `js_with_get_binding_value`
  already did. Ours stays: it behaves the same and also serves
  `with_get_ref_undef`.
- The deferred `ToPropertyKey` in assignments, now upstream's:
  `strip_eager_propkey_conversion` and `emit_deferred_propkey_conversion` are
  gone. `get_lvalue` no longer emits the eager conversion, so the helpers had
  nothing to strip, and the store converts the key after the right-hand side.
  Evaluation order is unchanged for plain, compound, logical, destructuring,
  default, `for-in`/`for-of` and `super[key]` targets.
- The `DOMException` attributes, now upstream's: it makes `name`, `message`
  and `code` enumerable and the constructor's length 0, as our patch did; they
  are now defined in WebIDL order (`name`, `message`, `code`).

**Adaptation required.**

- `ns_js_interrupt_cb` in `src/js.c` takes an `ns_js_interrupt_arg`, which
  `src/ns_quickjs.h` defines as `JSContext` on quickjs-ng and `JSRuntime` on
  the original engine.
- `JS_ReadObject` also accepts `BC_VERSION` 27 data, reading the old
  four-byte predefined atoms, so IndexedDB records stored by an earlier build
  stay readable.
- `NS_BYTECODE_CACHE_FORMAT_VERSION` is bumped, so cached script bytecode is
  compiled again by the new compiler.

### Verification

- The GCC build, a clang build and a `-Dquickjs=quickjs` build compile and
  link with no new warnings.
- `./scripts/dev.sh smoke` passes on both engines. A page with an endless
  loop is interrupted after the eval budget on both engines, and the
  precompiled `Array.fromAsync`, `Iterator.zip` and `Iterator.zipKeyed` run.
- test262 at the submodule pin (`5ef1e57`) and at current main (`c8c7988`),
  run with `run-test262` built from the fork before and after the update:
  identical results, 11 and 23 failures, none new. `tests.conf` gives the
  same results before and after.
- Values written with `qjs:bjson` by the old engine (version 27) read back
  identically in the new one.

## 2026-09-27 — Lexbor de1d07a → 327a8b6

| | value |
|---|---|
| Fork base | `de1d07a` (master, "Test: added README.md") |
| Updated to | `327a8b6` (master, "URL: Fix parsing of query and fragment after a dot-component in path (#411)") |
| Version | `3.1.0` (unchanged upstream); URL module 0.5 → 0.6, encoding module 2.3 → 2.4 |

Twenty-six upstream commits; twenty-one touch the modules we carry.

- **Memory safety** — `lexbor_mraw_realloc()` put the freed tail of a shrunk
  non-tail allocation at the wrong offset, so later allocations could overlap
  live data (`f3f6fa7`); `lxb_dom_character_data_replace()` wrote past the end
  of its buffer (`cf31487`); IDNA buffer growth copied from uninitialized
  memory when it moved off the stack buffer (`b0f7412`); `lexbor_conv_*`
  integer and hex conversion ignored the output capacity (`05b5d37`).
- **URL** — the dot-segment rework (`327a8b6`): `.`/`..` followed by `?` or
  `#` keeps the delimiter on both the fast and the slow path, `path.length` no
  longer drifts (so `/a/b/../../../c/é/../../x` is `/x`), an invalid `%`
  sequence no longer swallows the following delimiter, `\` after a dot segment
  reports invalid-reverse-solidus, and the empty segment in `//./c` survives.
  Also: usernames containing `@` (`a36e09a`), spaces at the end of an opaque
  path are encoded before `?`/`#` (`1b215a8`), fragment serialization without
  a query (`385afff`), a replacement `file:` drive path stays hierarchical
  (`917742f`), and new public API — the IPv6 parser, the percent-encoder with
  its map, and component reset helpers (`a7dbebb`, `2b24b56`, `e6c068f`).
- **HTML** — a `<form>` inside template contents is inserted without touching
  the form element pointer when the template is the fragment-parsing context,
  in body and in table (new `lxb_html_tree_parsing_template_contents()`);
  "after head" resets frameset-ok; meta `charset`/`content` attribute names are
  matched exactly in the prescan (`61ef2bc`).
- **Encoding** — the UTF-8 single-code-point decoder returns CONTINUE on empty
  input, the valid-UTF-8 decoder rejects it, and the UTF-8 single encoder
  reports SMALL_BUFFER instead of writing an ASCII byte past `end` (`47be1dc`,
  `5d3a6f5`). The Shift_JIS, single-byte and UTF-16 fixes (`7314185`,
  `826b121`, `97c2aab`) land in codecs this fork no longer carries.
- **Utils** — HTTP header fields reject a bare CR and serialize CRLF
  (`1170290`).

**Local modifications preserved:** `unicode/idna*` (our IDNA layer, merged
cleanly with upstream's buffer-growth fix), `core/mraw.h` (unaligned-load
fix), the UTF-8-only encoding module, `url/url.c` (UTF-8-only query and
percent-encoding, and a port under a state override keeping the hostname), and
our in-tree `meson.build`. `lxb_url_percent_encode_after_encoding()` gained
upstream's `url_map` argument and still always encodes as UTF-8; upstream's new
`lxb_url_output_encoding()` helper is not carried, since only UTF-8 remains.

**Local modification dropped:** our dot-segment terminator fix in `url/url.c`
(`2b6f8b9`). Upstream's `327a8b6` fixes the same bug on both paths and the
related `path.length` and percent-sequence cases ours did not cover.

### Verification

- `meson compile -C builddir` — builds and links cleanly with no warnings.
- A headless page exercising the URL parser (dot segments on both paths,
  `@` in userinfo, opaque-path spaces, IDNA, the host setter with an invalid
  port, `file:` drive paths, IPv6, `URLSearchParams`), the HTML tree builder
  (form in template fragments and documents, form in table, adoption agency,
  processing instructions), `CharacterData.replaceData` and
  `TextEncoder`/`TextDecoder` gives the expected results, and larger local
  documents render the same text as the pre-refresh build.
- WPT's `url/resources` data (`urltestdata.json`, the JavaScript-only set,
  `setters_tests.json`, `toascii.json` and the host-only `IdnaTestV2.json`
  cases), driven headless through `URL`: 3855 → 3868 passing against the
  pre-refresh build, no case newly failing. The gains are spaces at the end of
  an opaque path before `?`/`#`, including after the `search` and `hash`
  setters clear them.

## 2026-08-08 — QuickJS e2c45218 → 954dc53, Lexbor cf07699 → de1d07a

### QuickJS

| | value |
|---|---|
| Fork base | `e2c45218` (master, "Fix integer multiplication cast to unsigned long in js_realloc_array()") |
| Updated to | `954dc53` (master, "Set version to 0.16.1") |
| Reported version | `0.15.1` → `0.16.1` (`QJS_VERSION_*` in `quickjs.h`) |

Nineteen upstream commits. The security fix is an out-of-bounds write in
libregexp's string-set emitter (`c2f37ec`): a `v`-flag class containing the
empty string, `/[\q{}]/v`, reached `memcmp`/`memcpy` with a NULL buffer, and
`re_emit_string_list` patched byte-code positions that a failed allocation had
never produced.

The language work is the iterator proposals — `Iterator.concat` returning a
proper iterator helper and closing correctly on a throwing value getter,
`Iterator.prototype.join`, `includes`, and the chunking proposal's `chunks` /
`windows`, with `take`/`drop` rejecting out-of-range limits — plus the throw
paths of the existing helpers. Elsewhere: externally managed ArrayBuffers can
now be resizable, the parser no longer rescans the line to find an
identifier's column, `libunicode-table.h` is reproducible from `unicode_gen`,
and `js_realloc2`'s allocation-slack feedback is gone.

**Local modifications preserved.** `quickjs.c`, `libregexp.c` and `quickjs.h`
all merged without a conflict, and the local delta against upstream is
byte-identical in content before and after — every browser hook listed in the
2026-06-26 entry, `JS_RepointArrayBuffer` through the regexp legacy captures,
carries over untouched.

**Adaptation required.** Upstream replaced the externally managed ArrayBuffer
free callback with a realloc-shaped one and gave `JS_NewArrayBuffer` a
`max_len` parameter:

- `JS_RepointArrayBuffer` tests `abuf->realloc_func` rather than the removed
  `free_func` to decide the memory is not quickjs's to manage.
- `src/wasm.c` (WASM linear memory) and `src/webgpu.c` (a mapped GPU buffer)
  pass `max_len = 0`, keeping both fixed-length; the WebGPU callback returns
  NULL for any non-zero size, which upstream reads as "cannot resize", so
  wgpu-owned memory is never moved or freed by the engine.

### Lexbor

| | value |
|---|---|
| Fork base | `cf07699` (main, "URL: fixed setters for empty hosts") |
| Updated to | `de1d07a` (main, "Test: added README.md") |
| Version | `3.1.0` (unchanged upstream) |

Fourteen upstream commits; three touch the modules we carry (the CSS::Syntax
and Style fixes are in modules this fork does not build).

- **URL** — a caret in a path is percent-encoded (#394), and a URL with
  userinfo but an empty host is rejected (#393). We had already made the caret
  fix locally, so the refresh takes upstream's spelling of it and the local
  delta shrinks accordingly.
- **HTML** — processing instructions, which the HTML Standard added after this
  fork was taken. `<?target data?>` now produces a real
  `lxb_dom_processing_instruction_t` instead of a bogus comment, with the
  specification's disallowed targets (`xml`, `xml-stylesheet`) still falling
  back to one.

**Local modifications preserved:** `unicode/idna*` (our IDNA layer),
`core/mraw.h` (unaligned-load fix), `url/url.c` (a port under a state override
keeps the hostname), and our in-tree `meson.build`.

**Local modification dropped:** the `LXB_HTML_TOKEN_TYPE_FROM_QM` token flag
and `lxb_dom_comment::from_bogus_qm`, which marked a comment as having come
from `<?` so that `src/html_lexbor.c` could scrape a target and data back out
of the comment text. Upstream's parser-level support supersedes it, and the
engine now reads the target and data directly off the DOM node.

### Verification

- `meson compile -C builddir` — the full browser builds and links cleanly with
  no new warnings.
- Headless runs over pages exercising both forks: the preserved QuickJS
  changes (`RegExp.$1`, the always-called sort comparator, the regexp `v`
  flag, `with`-object re-probe and `@@unscopables`, `Function.prototype.caller`),
  the new 0.16 iterator helpers, `WebAssembly.Memory` allocation and `grow`
  across the changed ArrayBuffer API, the processing-instruction node type,
  target, data and serialization, and the two URL fixes.

## 2026-07-28 — QuickJS 4d6fe60 → e2c45218

| | value |
|---|---|
| Fork base | `4d6fe60` (main, "Add Unicode license to libunicode-table.h", #1547) |
| Updated to | `e2c45218` (master, "Fix integer multiplication cast to unsigned long in js_realloc_array()") |
| Reported version | `0.15.1` (unchanged upstream; `QJS_VERSION_*` in `quickjs.h`) |

This refresh brings in 49 upstream commits. The security and robustness work
includes fixes for use-after-free in suspended coroutines, promise capability
creation, array growth and `DisposableStack`; double-free fixes for CallSite
and detached ArrayBuffer paths; integer-overflow fixes in Proxy `ownKeys`,
TypedArray sorting and array allocation; reference/leak fixes across Atomics,
iterators, JSON ropes and Set methods; hash-collision hardening; and a named
capture scope limit.

Other upstream changes include the arena allocator, the register-based regexp
engine, faster mixed numeric arithmetic and fast-array reads, full import
attribute module identity, TypedArray resize/overlap corrections, async
iterator closing fixes, improved module export/error behavior,
`CallSite.prototype.isConstructor()`, and corrected source positions.

The three-way merge preserves Nordstjernen's browser hooks and compatibility
work, including `JS_RepointArrayBuffer`, caller/function realm accessors,
browser-shaped stack traces and TypeErrors, native-facade formatting,
cross-frame caller handling, regexp legacy captures and `RGI_Emoji`, the
optimized prototype scan for `for…in`, and the local in-tree Meson build. The
local regexp string-property emitter was adapted to the new opcode set, and
the `for…in` optimization now accesses properties through the arena-aware
shape helper. The upstream async-iterator normal-completion fix was reconciled
with the local rejection-safe iterator stack layout. Upstream's generalized
import-attribute cache supersedes the local type-only cache while preserving
its behavior.

## 2026-06-26 — QuickJS 66adc82 → 4d6fe60, Lexbor 3.0.0 → 3.1.0

### QuickJS

| | value |
|---|---|
| Fork base | `66adc82` ("Only export API symbols for shared builds", #1525) |
| Updated to | `4d6fe60` (main, "Add Unicode license to libunicode-table.h", #1547) |
| Reported version | `0.15.1` (unchanged upstream; `QJS_VERSION_*` in `quickjs.h`) |

**Upstream changes pulled in** (15 commits since the fork base; those that
touch carried files):

- Implement the *nonextensible-applies-to-private* proposal.
- ArrayBuffer immutable-method semantics fixes (`sliceToImmutable` /
  `transferToImmutable` error types and coercion order).
- Enforce immutability on TypedArray write paths — adds a `require_mutable`
  argument to `js_typed_array___speciesCreate` and the related create paths.
- Implement the `Error.prototype.stack` accessor proposal.
- Add `JS_Free{Atoms,Values}` vararg macros (#1535).
- `-Wmaybe-uninitialized` fix in `js_binary_logic_slow`; test262, unicode
  table (+license), CI and meson-default-build housekeeping.

**Local modifications preserved** (`quickjs.c`, `quickjs.h`, `libregexp.c`):

- `JS_RepointArrayBuffer()` — repoint an externally-managed ArrayBuffer at a
  new backing store and update live views (browser-side hook).
- TypedArray `[[Set]]` only coerces the value when the receiver is the typed
  array itself (out-of-bounds index with a foreign receiver leaves the value
  untouched).
- `with`-statement object Environment Record semantics: `GetBindingValue` /
  `SetMutableBinding` re-probe the binding and raise `ReferenceError` in
  strict mode (`OP_with_get_var/_ref/_ref_undef/_put_var`).
- `Function.prototype.caller` getter resolving the live caller for
  non-strict functions (legacy web behaviour); `arguments` stays poisoned.
- Parser: `get`/`set` on its own line before a generator method is treated
  as a class field (ASI); computed property keys run through `ToPropertyKey`
  before the value expression.
- Module resolution: `export * as ns` ambiguity compared by canonical
  (module, binding) so the same namespace re-exported via different modules
  compares equal.
- Array sort always invokes a user comparator, even for bitwise-identical
  values (matches V8/JSC/SM; `jQuery.uniqueSort` relies on it).
- RegExp legacy static captures `RegExp.$1`…`$9`, updated on every successful
  `exec`.
- `%AsyncFromSyncIteratorPrototype%` closes the sync iterator on a rejected
  value promise (`closeOnRejection`).
- TypedArray `subarray` species-create omits the length argument (rather than
  passing `undefined`) for length-tracking results.
- `DOMException` prototype stored via `set_value` (drops a leaked reference).
- `libregexp.c`: the regexp `v` flag (unicodeSets) also enables full Unicode
  semantics (`is_unicode`).
- `quickjs.h`: `JS_EXTERN` / `JS_MODULE_EXTERN` only get default visibility
  for shared/module builds; `JS_RepointArrayBuffer` declaration.

The merge produced one conflict (subarray species-create) where our
length-omission edit met upstream's new `require_mutable` parameter; resolved
by keeping our branch and threading the new `false` (read-access) argument.

### Lexbor

| | value |
|---|---|
| Fork base | post-3.0.0 main (around `6772a05`) |
| Updated to | `cf07699` (main, "URL: fixed setters for empty hosts") |
| Version | `3.0.0` → `3.1.0` (`LEXBOR_VERSION_*`, `version`) |

**Upstream changes pulled in** (carried modules only):

- HTML: nobr adoption-agency fallback fix
  (`html/tree/insertion_mode/in_body.c`).
- URL: empty-host setter fixes — `lxb_url_host_is_empty()` helper, empty-host
  short-circuit in host copy/parse (`url/url.c`).
- DOM: expanded attribute constant/lookup tables
  (`dom/interfaces/attr_const.h`, `attr_res.h`).
- Version bump to 3.1.0 (`core/base.h`, `version`).

**Local modifications preserved:**

- `unicode/idna.c`, `unicode/idna.h`, `unicode/idna_validity.c` — our IDNA
  processing/validity layer (custom file plus heavy local edits).
- `core/mraw.h` — `lexbor_mraw_data_size` reads the size header via `memcpy`
  to avoid an unaligned load.
- `url/url.c` — port state under a state override returns `LXB_STATUS_OK`
  with an empty buffer, so `url.host = "example.com:invalid"` keeps the
  hostname instead of rejecting the assignment. This sits in a different
  region from upstream's empty-host fix and was combined cleanly by the
  3-way merge.
- `meson.build` — our in-tree build description (sources listed explicitly).

`url/url.c` merged with zero conflicts (our edit and upstream's empty-host
fix occupy disjoint regions); all other carried files were pure copy/keep.

### Verification

- `meson compile -C builddir` — full browser builds and links cleanly, no
  new warnings on the QuickJS/Lexbor objects.
- Headless run of a page whose inline script exercises the preserved engine
  changes: `RegExp.$1` static captures, always-called sort comparator, and
  the regexp `v` flag all behave correctly, and the Lexbor DOM dump parses
  `<select size>` / `<nobr>` as expected.
