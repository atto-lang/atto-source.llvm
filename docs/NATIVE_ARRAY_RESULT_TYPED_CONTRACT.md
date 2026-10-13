# Native typed Array result contract (stage 1)

**Status: host-only nominal closed-domain proof. This is NOT guest Result/match lowering or Java parity.**

The first closed specialization is `Result<void, ArrayError>` with exactly
three inhabitants: `Ok`, `Err(ArrayError.Limit)`, and
`Err(ArrayError.NoMemory)`. The runtime C statuses 0, 1, and 2 respectively
decode into nominal case identities. Status numbers are **never** a public
Oreslang integer API.

## Security and correctness

- Runtime decoding accepts **only** 0/1/2, without truncation. Invalid tags,
  including signed-negative values reinterpreted as unsigned, cannot become
  `Err`, be caught, or enter guest arithmetic. Reject after exactly-once
  owner cleanup as fatal invariants.
- The helper is allocation-free, `noexcept`, and leaves output unchanged
  when an invalid status is encountered.
- C++ `AppendResult` does not implicitly convert to `int`, `bool` or
  `uint32_t`. C++ layout/padding is **not** a guest ABI or serialization
  format; stable versioned nominal type descriptors remain outstanding.
- Source-level `Result` must be **must-use**, including never-read locals;
  arithmetic, implicit conversions and accidental status discard must fail
  compilation.
- An exclusive match is exhaustive only if every inhabitant is covered:
  `Ok` and `Err(Limit)` without `Err(NoMemory)` is incomplete.
  Overlapping/duplicate arms are rejected; the final wildcard covers only
  the complement. Guarded patterns require independent proof.
- A generic `Result<T,E>` with nontrivial payloads requires recursive
  exhaustiveness and ownership/drop-glue, beyond this finite specialization.

## Verification

`NativeArrayResultContract.hpp` and
`native_array_result_contract_test.cpp` are a **host-only** precursor.
The test covers all 256 byte-range statuses, high-bit/negative inputs,
result/noninteger type separation, overlapping and dead match arms, and 125
three-arm pattern combinations. CTest includes a dedicated host-only gate.

**Open:** LLVM source typechecker/parser, actual tagged `Result` guest
construction/storage/matching, `Result<void,E>` zero-payload `Ok`
constructor parity in Java, must-use and affine ownership, Java/GraalVM vs
LLVM same-source execution, native AOT sanitizers and full tip CTest.

Track LLVM issues #314, #316, #317 and draft checked-array PR #333.
