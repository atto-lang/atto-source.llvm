# Experimental inverted i64 guest compiler — source-to-LLVM-IR vertical slice

**Status:** Draft, not built on a stepful exact-head GitHub runner, not production grammar, not five-backend parity.

This intentionally separate native-host mode is invoked as:

```sh
oreslang-llvmc --experimental-inverted-i64 tests/fixtures/inverted-i64/positive-column-get.ores > /tmp/inverted.ll
clang -x ir /tmp/inverted.ll -o /tmp/inverted
/tmp/inverted
# expected exit status: 42
```

Only the explicit flag enables the guest subset; the default source compiler continues to deny unlowered `define inverted class` and `define inverted struct` with compiler-owned diagnostics. An experimental source must be one inverted value declaration followed by one `fnc main(): int` using exactly one local `InvertedArray<DeclaredStruct>.with_capacity(N)`.

Supported at this stage: 1–8 immediate named `int` fields, capacity 1–256 and maximum 1024 physical cells, initial `push(i64, ...)` with all fields present, checked index `set(index, field, i64)` *only for fields declared `let`*, and `return array.get(index, field)`. Declarations of plain `x: int;` use Oreslang's implicit immutable `val` member semantics: initialization through `push` is legal, later writes are denied. Both `pub define` and `define pub` are accepted.

The compiler **parses actual .ores source**, emits `alloca [N x i64]` individually for each immediate field, writes and reads via `getelementptr`, and checks `index < length` before any read or write; out-of-bounds operations trap through `llvm.trap`. `push` checks capacity and publishes the incremented logical length *after* initializing all column cells. All supplied values are validated signed-i64 literals before LLVM IR emission. No GC heap or ordinary array-of-structs is emitted in this mode. Physical columns are stack-allocated, bounded, and local to `main`, not actor-owned.

**Not implemented:** general nominal struct/Class AST integration, methods, nested packed records, variable expressions/indices, row handles, generation validation, `erase_swap`, borrowing, shared access, await/suspend, actor-domain storage, independent allocator, callbacks, dynamic memory, ownership transfer, wasm32 target, and real-time certification. None of these may silently fall back to an ordinary `Array<T>` or an untyped pointer. This experiment does **not** invoke or prove the host `DopInvertedColumns` class's handle/generation semantics; it implements its own limited, stack-resident column codegen as a pre-HIR proof.

The value returned by the guest is an Oreslang `int` (i64); the host process entry truncates to the native C `main` i32 exit convention, as used in the positive fixtures. The tests deliberately choose 42.

Tests: `tests/native_inverted_i64_guest.py` requires both the real `oreslang-llvmc` and a native Clang IR driver. Its two positive fixtures must compile to physical LLVM columns, link via Clang and execute with process status 42; an out-of-bounds guest must compile but trap; 11 invalid programs must fail before emitting an LLVM module. CTest registers `native_inverted_i64_guest` when Clang is found. `native_inverted_source_failclosed` continues to verify normal-mode denial. Shared copied fixture blobs are pinned in conformance PR #249's `contracts/experiments/inverted-i64/manifest.json`.

The PR does not assert these CTest jobs executed. Hosted Actions on the source org continue to fail at runner admission before steps. No private source is transferred to an alternate GitHub organization. Before promotion, require stepful exact-head GCC/Clang C++20 build, Clang LLVM-IR compile/link/execute, CI transcript, and independent review; then replace this bootstrap with the common ownership-checked typed HIR/MIR lowering.

## Experimental wider-column owner revocation (additional draft gate)

The legacy 1/3..8 i64-column code path is **not** the independently checked two-i64 MIR-to-LLVM emitter. It remains explicitly experimental and cannot establish general inverted types or production guest parity. It now closes its typed MIR owner before publishing LLVM IR; the emitted local function copies its return scalar, revokes owner state with a volatile store, scrubs all `capacity × field_count` physical cells, clears logical length and returns only afterward. This is safe teardown for local scalar-only columns, **not** support for generational row handles, field loans, variable indices, nested packed structs, actors, suspension or other backends.

New `.atto` cases `positive-one-column-close.atto`, `positive-three-column-close.atto`, `positive-eight-column-close.atto` and `negative-three-column-bounds.atto` extend the native guest harness. They require correct one-level column count, volatile scrub count, Clang O0/O2 native exit 42 on positives, and an actual SIGILL/SIGTRAP on the bounds-negative. Current branch evidence status: **CODE_STAGED / EXACT-HEAD HOSTED TESTS NOT_EXECUTED** until an authorized stepful runner compiles and executes the real source compiler and guests. Existing `*.ores` conformance blobs are unchanged and no backend parity flags are advanced.
