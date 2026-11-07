# Restricted AttoLang f64 runtime observation (LLVM draft v1)

This is an **isolated draft stacked on LLVM PR #1488**. It does not alter
the parent branch or claim the complete four-layout DOP ABI.

## Source expression admitted

Within the already-admitted standalone `i64/bool/f64`
`define inverted struct` / local `fnc main(): int` slice, an ordinary
typed terminal expression is admitted:

```atto
return rows.get(0, energy) == 3.75 ? 42 : 7;
```

The `energy` field must have exactly `f64` type. The comparator must be
a **finite explicitly floating-point literal** (`3.75`, `0.0`,
`-0.0` etc.). Each ternary branch is a signed `i64` literal.
The terminal expression is lowered into an actual checked `f64` load,
IEEE-754 **ordered equality** (`fcmp oeq double`) and an integer
`select`. It is not an implicit `f64`-to-integer conversion. Ordinary
terminal `return rows.get(i, integer_field)` stays unchanged.

This does **not** admit general arithmetic, dynamic row indices, callable
closures, tenant objects, escaped pointers, suspension, actors, general
`f64` return values or `!=`/other comparisons. Rows remain bounded
literal-index, stack-local owners with independently verified guards,
swap-delete, typed write permissions, and volatile close scrubbing.

IEEE numeric equality is intentional: `-0.0 == +0.0` is true. The
existing independent raw-bit regression checks that storing `-0.0`
keeps its IEEE sign bit in LLVM IR. A future bit-identity API needs
a different explicitly named operation, not a surprising change to
numeric `==`.

## Acceptance matrix (all still unqualified until executed)

- Two original mixed three-scalar positive guests and three additional
  runtime `f64` comparison guests. Compile all five through the **real**
  source-owned `oreslang-llvmc`, audit source IR for no-GC imports, then
  run each under Clang `-O0` and `-O2`: **10 exit-42 cases**.
- One original stale i64 dense-index fixture and one checked stale
  `f64` index fixture, each at O0/O2: **4 real signal traps**.
  These indices are emitted through `read_runtime_checked` and must not
  be silently classified as compile-time errors.
- Five original negative mixed sources plus an invalid `i64`-as-`f64`
  comparator and a `bool`-field comparator: **7 fail-closed compiler
  rejections** with no partial IR emitted.
- Required LLVM IR markers include a real `load double`, `fcmp oeq
  double`, `select i1`, owner/bounds/slot guards, and the existing
  volatile teardown.
- The existing two-i64 production native and MIR cases, mixed MIR case,
  production AST parser case and mixed native case all remain required.
  Legacy verification additionally rejects nonzero hidden comparison
  metadata from forged/deserialized source operations.
- The exact-head job must attest runner allocation, nonzero executed
  steps, CTest JUnit status/duration/markers, SHA-256 of all **14**
  `.atto` fixtures, production compiler and no-GC auditor, and stable
  Git head/tree before and after test execution.

Until these conditions actually run and pass on a permitted source-owned
runner, this PR is **AUTHORED / GUEST NOT_EXECUTED**. A hand-authored Clang
IR smoke proves only the fcmp/select instruction form, not that the
production parser, typed MIR, or native guest works. Independent approval
and parent-chain reconciliation are required before readiness or merge.
