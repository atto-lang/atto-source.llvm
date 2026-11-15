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

## Canonical, versioned closed-result transport (host codec only)

The closed specialization has a portable **three-byte** v1 encoding:
`[version=1, variant, error]`. Its only accepted byte sequences are
`01 00 00` (Ok), `01 01 01` (Err(Limit)), and `01 01 02`
(Err(NoMemory)). A success carries a **zero** error byte, not stale
memory; a failure always carries a nominal ArrayError tag. All other
representations, lengths, unknown versions, and null inputs fail decoding
without modifying the caller's output. Encoding likewise refuses forged
variant/error discriminants, including stale error data in an Ok host value,
before writing any output bytes. The internal C++ success value stores a zero
absence marker; this is **not** an additional public ArrayError variant. Neither
C++ struct layout nor byte-order-dependent padding enters this format.

The 65,536 variant/error byte pairs are exhaustively tested. This
format is a **host-level wire proposal for this closed specialization**,
not proof that the Java/GraalVM guest compiler accepts it, nor a general
`Result<T,E>` representation. Version negotiation and a shared Java
implementation must be verified before exposing it as a stable wire API.

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
