# Oreslang ownership syntax and allocation semantics

**Status:** shared language contract. Compiler backends may implement only
subsets; unsupported semantics must reject without emitting executable code.
This document does **not** claim that full ownership/borrow/drop parity exists.

## No public `owned` keyword

- **`owned` is not a reserved Oreslang keyword, a type constructor, or a
  required storage qualifier.** The Java/Graal grammar has `val`, `let`
  and `const` bindings with ordinary type references, and does not define
  `owned T`.
- A by-value binding/parameter/result implicitly owns its value. For
  `Copy` types (including `int`) by-value operations copy; for
  non-`Copy` types they move. A value's ownership must **not** require heap
  boxing, a native arena allocation, tracing GC, or an actor.
- The identifier `owned` remains legal in name positions such as
  `const owned: int = 6;`, unless a separate explicitly specified
  language-scoped restriction is approved with migration tests.
- Existing explicit ownership operations use the agreed contracts:
  `rt borrow`, `rt borrow mut`, `rt take`, `rt copy`,
  `rt share`, and `borrow T` / `borrow mut T` in type positions.
  An implementation may reject unsupported operations, but may not quietly
  reinterpret `owned int` as the canonical spelling of implicit ownership.

## Safety and speed are independent of storage

An owning value may live in a register, a stack slot, an actor-owned arena,
a scoped native allocation, or an explicitly shared region. Storage selection
is a compiler/runtime lowering decision constrained by escape/lifetime,
access and capability rules—not source-level `owned` spelling.

- Scalar and small `Copy` values should be stack/register/inline eligible.
  A native allocator must **not** be invoked merely because a local is owned.
- Non-`Copy` values must be moved or safely borrowed; required cleanup runs
  deterministically on each admitted control-flow exit.
- Shared/isolated actors control concurrency and isolation; neither introduces
  a new `owned` type nor requires one heap allocation per field/message.
- LLVM strict native mode may not introduce a tracing collector. Java/Graal
  may use host-managed implementation allocations, but memory safety and
  resource closure must not depend on their GC timing.

## LLVM prototype carve-out

LLVM's experimental scalar-native region prototypes used a test-only
`owned int` source spelling to exercise allocation, native drop glue and
per-return cleanup. **This is not language admission or Java parity.**

The experimental compiler must reject that spelling by default; a
clearly named explicit test-mode flag can allow it for backend validation.
The test-mode behavior must not affect normal compilation or make the
`owned` identifier a global reserved word. Normal scalar programs should
not declare or link the experimental native heap ABI.

Related implementation: [LLVM PR #72](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/72).

## Cross-backend conformance fixtures

| Fixture / scenario | Standard expected result |
| --- | --- |
| `const value: int = 42;` | Accepted, Copy value, no mandatory heap allocation |
| `const owned: int = 6;` | Accepted; `owned` is a binding identifier |
| `const value: owned int = 42;` | Rejected as non-standard type spelling |
| `int` by-value arguments and return | Copy semantics, no mandatory heap |
| Non-`Copy` parameter by value | Moves the unique owner, caller no longer retains it |
| Explicit borrow of an owned value | Checked shared/exclusive loan, no owner transfer |
| Unsafe move/drop across await, actor send, FFI or cancellation | Rejected until valid lifetime/drop proof exists |

Backends must report feature support, exact compiler revisions, source
fixtures, executable evidence and any experimental flags. Do **not** count
experimental test mode as public language parity. An IR string check alone
is not proof of zero-GC linked-runtime behavior or of optimized code speed.

See [shared conformance](CONFORMANCE.md) and the separate
[draft zero-GC native / low-GC Graal contract PR #10](https://github.com/ores-truffle-oreslang/oreslang-common-source/pull/10)
for the proposed runtime/performance acceptance criteria.

See [ownership boundary audit](OWNERSHIP_BOUNDARY_AUDIT.md) for suspension,
critical state loan, runtime operation and effect-contract admission rules.
