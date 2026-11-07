# AttoLang native mixed-scalar inverted guest — restricted production v1

This is a **compiler-owned source-to-LLVM guest path**, intentionally separate
from the already-authored two-`i64` production path and from physical-layout
host-only ABI proposals. It is a stacked draft based on LLVM mixed-width
physical-capacity preflight #1471, itself stacked on source-main #1403.

## Executable source contract (narrow)

A standalone `.atto` file may contain exactly one
`define inverted struct` with **three fields** in any order:
one `int`/`i64`, one `bool`, and one `f64`.

One zero-argument `fnc main(): int` creates a local
`InvertedArray<Nominal>.with_capacity(N)`, with literal `1..256` and at
most 1024 literal-only operations. It may perform `push`, permitted `set`,
`erase_swap`, and one terminal `return rows.get(index, integer_field)`.
`let` permits field writes; unmarked fields stay immutable.

There are **no implicit conversions**:
- `i64` fields require integer literals.
- `bool` fields require `true` or `false`.
- `f64` fields require explicitly lexed finite binary real literals such as
  `1.5`, `-2.25` or `3.75`. Integer literals do not silently become f64.
  Native decimal suffix `d` and digit separators remain unsupported here.
- `main(): int` may return only the i64 field, not bool/f64.

All unsupported source features reject **before producing LLVM IR**:
functions, imports, escaped stores/row handles, methods, actors, async
suspension, source borrowing, snapshots, unsupported native options,
multiple schemas, dynamic expressions, and general inverted classes.

## Memory and safety contract

The source AST preserves the three types and typed literals; a separate HIR
and independently reverified MIR admit operation order, field mutability,
owner lifetime, capacity and signed indices. The native LLVM emitter allocates
three **physically separate, typed stack columns** (`i64`, `i1`, `double`)
plus a live-slot metadata column.

Each read, write, and erase is guarded by owner liveness, *logical* length,
and live metadata before address formation. Swap-delete copies all three
columns. Close revokes the owner and volatile-scrubs *every* physical cell
and live marker, even unused capacity. A final i64 read is captured before
close and truncated to the narrow native main exit ABI.

This is **not** an opaque generational RowHandle: indexes are re-resolved as
dense coordinates. The live marker is not a durable identity generation.
It is also **not** a proof of actor-private heaps, zero-GC linked binaries,
SIMD acceleration, AoSoA/SoAoS, cross-backend parity, or all f64 IEEE edge
cases. No guest-visible f64 field projection into an f64 expression exists
yet; positive guests check the emitted f64 column/literal and native main's
integer result, not numerical f64 reduction.

## Required executed evidence

The source-owned workflow
`.github/workflows/dop-inverted-mixed-native.yml` must:

1. Check out and attest the **immutable PR head/tree** (not the synthetic
   merge ref) and a clean worktree.
2. Build actual `oreslang-llvmc`, native parser AST test, strict C++17
   mixed MIR test, and source-built no-GC IR auditor.
3. Execute `native_inverted_mixed_source_to_native`: **four** expected
   exit-42 runs (two positive `.atto` sources, Clang O0/O2), **two** real
   SIGILL/SIGTRAP bounds runs (one negative `.atto`, O0/O2), **five**
   fail-closed source negatives, and no-GC source-IR verification.
4. Execute `inverted_mixed_source_mir_cpp17` and the production
   `native_inverted_struct_schema_ast` test.
5. Record nonzero GitHub job steps, CTest JUnit with all actual test cases,
   source/compiled hashes and build metadata, obtain independent review.

Authoring a workflow or fixture, a source blob hash, source-free C++/JS
tests, or `steps=null` is **NOT_EXECUTED guest evidence**.

Do not merge or mark ready before exact-head source-owned stepful execution,
independent approval and resolution of platform runner-admission issue
[oreslang-platform-conformance #315](https://github.com/ores-truffle-oreslang/oreslang-platform-conformance/issues/315).
No cross-organization private-source mirroring is authorized by this draft.
