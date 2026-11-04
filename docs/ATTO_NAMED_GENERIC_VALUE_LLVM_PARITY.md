# AttoLang named generics + named value calls: LLVM native scalar slice

Status: **draft implementation and compiler-backed CTest targets**, not a verified
cross-backend guest parity receipt. Java and the shared conformance repository
have broader syntax/semantic targets. The compiler's existing `.ores` extension
is retained for compatibility; the independent `.atto` manifest is a target,
not proof that this native frontend presently accepts `.atto` paths.

## Working subset targeted by this patch

```atto
fnc encode<A, B=int, C=B>(A first, B second): int {
  return first * 10 + second;
}
pub fnc main(): int {
  return encode<C=int, A=int>(second: 2, first: 4);
}
```

There are **two separate binding maps**. `<C=int, A=int>` chooses
compile-time generic slots by case-sensitive name. `(second: 2, first: 4)`
maps source-evaluated values to declaration ABI slots. Neither map may
select an overload by label or runtime type. Actuals are evaluated in source
order and then reordered to formal ABI order.

This branch adds a strict scalar type check for every generic formal:
all admitted generic concrete values are `int`/LLVM i64 in this first
native slice. The previous `parameter_is_scalar` check skipped
`parameter_generic_types`, risking a boolean (i1) or string-typed actual
being passed to an i64 formal. A second LLVM emitter guard rejects any
mismatch between evaluated LLVM actual types and declared callee formal
types before emitting `call`. These checks use the already-bound formal
slot, never the original source index.

## Explicit support limits

| Construct | LLVM native status / gate |
| --- | --- |
| `fnc f<T>(T a)`, `routine f<T>(T a)` | Scalar i64 candidate |
| `f<T=int>(a: 42)` | Combined generic/value binder candidate |
| Named generic order, default from earlier slot, `_` inference | Scalar i64 only |
| Named runtime values in arbitrary source order | Compiler-checked exact required slots |
| Multiple same-name/different-arity functions | Select by name/arity, not labels |
| Generic method declarations and class type instantiation | Fail closed; nominal ABI not specialized |
| Named Array/uncertified member calls | Fail closed on parent LLVM #1228 |
| `T?` optional runtime argument omission, `@Default` | Fail closed; no presence/Option ABI |
| Non-i64 generic actuals: bool/string/classes | Fail closed |
| Move/borrow/actor/suspension generic crossing | Existing strict owner/effect gates |
| Generic `Any`/`Unknown` fallback and implicit conversion | Not admitted |
| LLVM `.atto` source admission | Not certified by these `.ores` CTests |

## Executable evidence gates

The focused workflow `.github/workflows/named-call-exact-head.yml`
checks the exact Git commit/tree, builds `oreslang-llvmc` and its Rust
test harness, requires all named-call CTests in the inventory, and runs
the same selected suite. Its newly authored 15 tests comprise:

- **5 positive native linked guest executions**: combined named generics
  + values, a direct-only routine, explicit inference hole, positional
  prefix, and reordered source-effect case (expected exits 42/42/42/42/12).
- **1 real LLVM IR-order assertion**, checking `read_second` precedes
  `read_first` after named binding.
- **9 negative real-compiler source checks** requiring rejection/diagnostic
  before LLVM IR: bool/string type erasure, reordered bool actual,
  duplicated/unknown/ordered generic slots, value-slot collision,
  unknown value label, and uninferable hole.

These 15 cases are additive to previously registered named-call and
member-call tests. A test listing, authored test source, zero-step job, or
host binder oracle does not establish execution. For promotion require
exact-head source/tree attestation, nonzero executed job steps,
`ctest --output-on-failure` pass counts, LLVM IR emission and Clang
link + guest return receipts, negative diagnostics without emitted IR,
and independent review. Repeat with Release/Debug and sanitizers where
applicable. Keep private source in the owning organization until separately
authorized transfer policy is reviewed and approved.

## Cross-backend progression

Shared conformance `oreslang-platform-conformance` PR #345 expands the
source-target-only corpus to 40 `.atto` cases (18 accept/22 reject). Its
backend result fields are intentionally `pending`. Java #2507 covers
generic declaration/source binding; Java #2508 is a review of
identity-bound named call optimizer proofs stacked on Java #2086.
LLVM #1228 is a prerequisite for member-call fail-closed parsing.

The next native milestones are explicit `.atto` entry-point admission,
stable compile-time generic specialization identity beyond i64,
proper generic method/constructor ABI, optional presence metadata and
call ownership/effect checks across both labeled maps. These must
not be promoted by this scalar-only patch.
