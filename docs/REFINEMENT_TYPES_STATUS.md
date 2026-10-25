# Refinement types: LLVM/native status

**Status (2026-10-25): bounded descriptor support; broader refinement lowering remains fail-closed.**

The canonical shared contract lives in `oreslang-common-source/docs/REFINEMENT_TYPES.md`.
This backend now has a deliberately narrow implemented slice from LLVM PR #969:

- `declare type Code = int with min 0 and max 5;` and the equivalent
  `define type` form are accepted as compile-time type declarations.
- Direct `int`/`i64` inclusive `min`/`max` predicates normalize into the
  compiler-owned `ores.type-constraint.v1` descriptor.
- Descriptor identity, canonical ordering, validation-required state and
  fingerprint are compiler-owned metadata.
- A constrained alias still **cannot become a runtime value**. Parameters,
  returns, alias chains and extension receivers that would erase validation
  provenance fail closed with `CONSTRAINT_VALIDATION_BRIDGE_REQUIRED` or the
  corresponding type-only signature bridge diagnostic.

This is not general refinement-type support. The native backend still rejects
annotation refinements, `where` predicates, nominal `newtype`, general
`with value ...` predicates, string/Unicode refinements, user-forgeable
`Valid<T, Schema>` proof types, arbitrary schema-first inference, and any
other surface without checked lowering.

## Required safety boundary

Unsupported source must fail with a nonzero compiler result, a diagnostic, and
**no partial LLVM IR/stdout**. The current admission regression includes an
ordinary scalar positive control, a positive `ores.type-constraint.v1`
descriptor control, the exact runtime-validation bridge denial, and broader
unsupported refinement forms.

Future checked construction must bind an immutable compiler/runtime validation
receipt to the exact nominal constrained type. Dynamic Zorde reports and wire
schema fingerprints are not sufficient authority by themselves.

String length semantics, when eventually implemented, are Unicode scalar
values rather than UTF-8 byte length. No backend-specific coercion may weaken
the shared contract.

This document and the regression gate describe admission semantics only.
Source-org zero-step Actions failures are infrastructure non-execution, not
evidence that LLVM/Clang compilation or CTest passed.
