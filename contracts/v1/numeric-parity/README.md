# Numerical parity admission contract v1

All five private repos track the same immutable fixture blob
`cb28b5678d380f02c5e4e43bd763da04bdb996fe`, first staged on Java
PR #1033 and LLVM PR #427. Copy it without changing a byte.

## Shared real-source i64 corpus

`source-i64-cases.json` has Git blob
`bbc319c19ecd007109d33592e210091eb267e5a2`: seven Oreslang
signed-i64 source expressions (including both signed extrema), with
exact 64-bit reference results. A separate Python oracle checks the
immutable fixtures, **not compiled source**. Implemented guest adapters:

- Java `NumericPortableI64GuestTest`: execute all seven in GraalVM.
- LLVM `native_numeric_portable_i64_source.py`: compile real
  Oreslang -> LLVM IR -> Clang, link a C launcher, verify the complete
  signed-64-bit value rather than the 8-bit process exit code.
- .NET `verification/Ores.Net.Tests/Program.cs`: real source parser,
  typed i64 IR, and reference evaluator (Native AOT execution separate).
- Forge `verification/tests.cpp`: real source parser, checked IR,
  interpreter (assembly emitter remains constant-return bootstrap).
- Wasm `tests/numeric-source-parity.test.mjs`: pin the actual LLVM
  compiler executable, compile source to wasm32, execute with zero host
  capabilities and verify full-width `BigInt` results; **skips when the
  real compiler is not supplied**, and a skip never counts as parity.

Actual-feature parity is **not established**: Actions attempts in the
source org have zero executed steps, and the bootstrap engines still
lack f32, f64, BigInt, Decimal, Complex, and Matrix implementations.
Cross-repo numerical work is tracked at
https://github.com/ores-truffle-oreslang/oreslang-source.java/issues/1068.

## Experimental LLVM checked-i64 execution slice

The compiler now admits `--target=host --experimental-checked-i64-functions`
for a **whole-module, pure, value-only signed-i64 function graph**, limited to
64 functions, eight parameters each, at most 2048 expression AST nodes
per function (depth under 64), and acyclic call depth at most 32. The
admission check rejects actors, classes/owned resources, locals, mutable
state, deferred effects, cross-target execution, generic arguments, unresolved
callees, recursive cycles and overload-arity collisions.

For this **opt-in** subset, signed add/subtract/multiply/unary negation use
`llvm.sadd.with.overflow.i64`, `llvm.ssub.with.overflow.i64` and
`llvm.smul.with.overflow.i64`. Overflow executes `llvm.trap`; it does **not**
return a Result, trigger Oreslang throw/raise, or clean up an owner frame.
The old experimental `--experimental-checked-i64` status C ABI remains
separate and cannot be combined with the new mode.

The CTest `numeric_experimental_checked_pure_function_graph` compiles
Oreslang source with the actual LLVM frontend, compiles the resulting IR
with Clang, executes 11×11 boundary inputs on each of add/sub/mul plus
nested-call and negation checks, and rejects unsupported effectful/cyclic
programs. This is **only test coverage added**, not a claim of an executed
exact-source CI pass. Default native arithmetic, actors and owner-bearing
functions remain **blocked_unsound** for signed overflow until a typed
failure-propagation ABI and cleanup/supervision design is implemented.

## Recoverable experimental checked-result function ABI (LLVM)

The separate opt-in `--target=host --experimental-checked-i64-result-functions`
emits C-callable symbols `__oreslang_checked_v2_<function>_<arity>` only
for explicitly `pub` functions, with signature
`i32(i64...arguments, i64* output)`. Private helpers retain
`llvm::Function::PrivateLinkage` and remain callable only from
other functions in the generated module. Pure modules without at least
one `pub` entrypoint are rejected before IR emission. Status `0` publishes exactly
one result, `1` denotes signed-i64 overflow, and `2` rejects a null output
pointer without evaluating arithmetic. A failing nested callee returns its
original status to the caller; the caller's output remains unchanged. This
uses real signed LLVM add/sub/mul/negate overflow intrinsics, *not* process
traps. The existing trap-only and single-function status v1 experiments
remain unchanged and cannot be combined with result v2.

**This is an internal migration proof, not general Oreslang semantics.** The
whole-module pure-function admission gate still rejects actors, classes,
owners, borrows, stateful code, effects, source-level errors and suspension.
It does not implement cleanup, supervision, a guest Result type, or portable
cross-backend public function ABI. Named call labels are not accepted in v2.

The source-to-ABI visibility contract is now checked explicitly:
`pub fnc` exports its result ABI entrypoint, while an ordinary `fnc`
used as an internal callee is lowered with `PrivateLinkage`. The guest
regression asserts both symbol types and fails if private helpers accidentally
become C-callable. All negative fixtures, except the dedicated private-only
rejection case, declare public entrypoints so one admission failure cannot
mask unrelated failures. Named-call, recursion, and missing-public cases
also verify specific diagnostics.

An independent **source-free** alternate-org LLVM shared-library test,
[run #38949404481](https://github.com/cliptown/cliptown-e2e/actions/runs/38949404481),
passed checked-result arithmetic at `-O0` and `-O2` (371 controls at
each level) plus dynamic-symbol visibility and nested-error checks for
private helpers at both optimization levels. These tests validate LLVM
linkage behavior, **not the real Oreslang compiler's emitted IR**. No private
source was transferred; the approval blocker remains Java issue #962.

The CTest `numeric_experimental_checked_result_function_graph` compiles
the guest source through the actual Oreslang LLVM compiler and links the IR
using Clang at `-O0`, `-O2`, and `-O3`. Its 11-point signed-i64 boundary
matrix exercises add/sub/mul, nested-call failures, intermediate overflow,
unary negation and two-step calls in **627 in-process assertions per
optimization level**, with no subprocess spawned for each boundary value.
It also performs **30,720** additional guest-function checks across all
three optimization levels from 2,048 deterministic full-width input pairs
per optimization level.
The bulk differential oracle uses signed `__int128` arithmetic to avoid
signed-i64 overflow in the test itself, and validates output immutability on
failure. Unsupported named-call lowering is denied before IR emission;
incompatible owned-int/scalar-loop flags are denied before reading source.

The same branch's `.github/workflows/ci.yml` now pins and verifies
the exact immutable PR-head commit/tree (rather than the default synthetic
PR merge checkout) and runs all three real compiler numeric CTests immediately
after building `oreslang-llvmc`, before the broad inventory and sanitizer
suite. The last CI run has zero executed steps and zero billed runner time;
these new gates have not executed in the source organization. A standalone
host-only reproduction of the 627-case boundary oracle passed with Clang
and UBSan at `-O0`, `-O2`, and `-O3`; it did **not** compile Oreslang source.

Independent source-free LLVM intrinsic/trap CI passed at all three
optimization levels: [run #38945879779](https://github.com/cliptown/cliptown-e2e/actions/runs/38945879779),
273 boundary cases per level (including ten isolated overflow signal checks),
819 cases total. This was **not** an Oreslang source compilation or permission
to copy private source. The slow old subprocess-per-overflow workflow was
replaced with bounded signal-fork tests and switched to manual invocation to
avoid wasting GitHub Actions minutes. Until the actual Oreslang compiler CTest
runs on an authorized runner, the new mode remains
`implemented_unverified` and must not advance the global default
`checked_i64_overflow` capability.

## Evidence is tiered

- `reference_only`: Python checks the independently defined fixture; **no Oreslang execution**.
- `implemented_unverified`: code exists, but exact-head compiler/guest CI has not passed.
- `delegated_unverified`: Wasm relies on the external native LLVM compiler; a
  passing Node sandbox test alone says nothing about numeric source semantics.
- `unsupported_explicit`: no guest implementation; reject unsupported source
  instead of lowering through binary64 or another incompatible domain.
- `blocked_unsound`: a known mismatch (e.g. Java source f32 aliases float).
- `needs_audit`: behavior not yet established against cross-backend contract.
- `verified_compiled`: **currently forbidden** by the fail-closed manifest gate.
  A self-declared workflow URL and case counts in the same PR are not a trusted
  proof. Implement independently approved/protected evidence verification that
  authenticates source and runner commits, actual executed positive/negative
  job steps and both fixture SHAs before enabling this state.

No backend currently declares `verified_compiled` for these features.
These manifests document current gaps, not fulfilled parity. Run:

```sh
python3 -m unittest discover -s tests -p 'test_numeric_*.py' -v
```

## Required implementation order

1. Agree on signed-i64 overflow/trap behavior, binary64 NaN/signed-zero,
   integer/floating comparisons, and source int/int division.
2. Implement real f32 and f64 typed lowering; forbid implicit BigInt/Decimal
   narrowing; match IEEE special-value rules.
3. Implement resource-bounded owned BigInt and Decimal in each native backend.
4. Add deterministic exact-accumulator `Matrix<Decimal>` with ownership
   and explicit rounding policy.
5. Run all fixtures through **actual** Java/GraalVM, LLVM, .NET Native AOT,
   OresForge, and real LLVM->wasm32 guest execution. No mock compiler can pass.
6. Retain explicit unsupported-feature negatives on bootstrap backends until
   native implementation is ready.

The .NET and Forge engines currently implement small checked-i64 subsets;
Wasm is LLVM-dependent and exposes only a core `() -> i64` ABI. Neither
running the reference Python oracle nor checking these manifests establishes
numerical feature parity.
