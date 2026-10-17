# Refinement types: LLVM parity contract (proposed)

**Status (2026-10-17): design only; no LLVM parsing/lowering/runtime enforcement claimed.**

The shared source contract is [oreslang-common-source/docs/REFINEMENT_TYPES.md](https://github.com/ores-truffle-oreslang/oreslang-common-source/blob/design/refinement-types-three-syntaxes-20261017/docs/REFINEMENT_TYPES.md).

The current native compiler remains an experimental subset and must **fail closed** when encountering any constrained type or newtype that it cannot prove and enforce. Merely parsing, erasing an annotation, or emitting `i64`/opaque pointers is **not** feature parity.

## Target source spellings

~~~ores
@Size(min=10, max=10)
type A = string;
type B = string with length eq 10;
type C = string where it.length == 10;
define newtype D(string) with length eq 10;
~~~

- All constrained aliases normalize to one typed predicate representation; `D` additionally has a distinct nominal ID.
- `with` accepts only recognized properties and conjunctive constant comparisons. Canonical textual comparators are `eq/ne/gt/gte/lt/lte`; symbolic aliases are `==/!=/>/>=/</<=`. `ge` and `le` are rejected. `where` accepts only pure deterministic expressions and never silently permits unvalidated values.
- Checked `Name.try_from(Base) -> Result<Name, ConstraintError>` construction is needed for runtime values.
- String length means Unicode **code points**, never UTF-8 bytes or Java UTF-16 units. NFC/NFD normalization is not implicit.
- Length/size-constrained mutable references cannot be mutated or aliased unsafely.
- Incomplete native parser and verifier must produce explicit errors, nonzero exit and **no emitted LLVM IR**, following the existing native fail-closed policy.
- No claim of parity until LLVM+Clang runtime tests demonstrate real checked conversion, nominal rejection, Unicode correctness, unsafe boundary validation and source/ABI compatibility with Java.

## Required CI evidence

1. Shared fixture corpus covering all three syntax families plus `where` and both textual/symbolic comparison tokens.
2. Static positive/negative cases, especially contradictions, zero-length invalid values, unsupported properties and unsafe conversions.
3. LLVM IR verification; real compiled runtime checks; absence of IR for rejected programs.
4. Differential Java/LLVM semantic results for Unicode, error shape, newtype identity, mutation and serializer/actor boundary behavior.
5. Clearly reported implemented versus explicitly rejected cases; no success counts awarded to skipped tests or tests that only exercise lexer rejection.

## Zorde runtime parity

LLVM refinement support includes emitting/consuming the same reflection-free Zorde validator descriptor contract as the language spec. Dynamic construction, deserialization, FFI, actor DATA and other trust boundaries must execute the normalized predicate or reject the unsupported descriptor before IR/execution. Backend-specific validation semantics are not permitted.
