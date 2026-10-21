# Trusted raw pointers for native / embedded Oreslang

Status: **native runtime foundation + fail-closed source gate**, 2026-10-17.

Oreslang source does not expose `Pointer<T>`, `RawPointer<T>`, address-of,
dereference, or pointer-arithmetic syntax. This branch's raw-pointer work is a
trusted compiler/runtime substrate only. `TrustedRawPointer<T>` may be used by
the native backend, core runtime, or platform/stdlib implementation boundary;
guest source cannot construct, name, cast, dereference, or perform arithmetic on it.

## What is implemented on this branch

- `FunctionEffect::UnsafeMemory` and canonical effect spelling
  `unsafe.memory` are part of the native effect bitset.
- `TrustedRawPointer<T>` provides a no-tracing, allocation-free native proof
  object for trivially-copyable element types.
- Bounded pointers created from existing spans require an explicit nonzero
  compiler/runtime provenance token; a numeric address is never substituted for
  region identity. They retain element count, address space and current index.
- Element arithmetic checks signed underflow/overflow and bounded OOB.
- One-past-end addresses are representable for comparison/difference but cannot
  be loaded or stored.
- Pointer subtraction requires bounded compiler provenance plus the same
  provenance/address-space and returns an element distance. Integer-fabricated
  pointers cannot be subtracted merely because their numeric addresses match.
- Numeric-address raw pointers require an immutable supervisor/platform
  `TrustedRawAddressGrant`; the integer itself carries no authority. The
  resulting view is range-bounded by the grant and inherits its address space.
  It still has no allocation provenance, so pointer subtraction remains
  forbidden.
- Ordinary and volatile load/store paths are distinct so MMIO code is not
  accidentally compiled as non-volatile access.
- The source parser rejects `rt ref`, `rt deref`, `rt unref` and `rt raw`
  as invalid Oreslang source before IR. This is a permanent source-language
  boundary, not a placeholder for future guest pointer lowering.

## Trust and actor rules

Raw-address authority is trusted implementation state, not a guest capability.
The backend/runtime must bind it to an approved core/platform operation,
allocation or device provenance, address space, generation/epoch and explicit
read/write authority. It must never be actor-mailbox, serialization, `Send`,
`Share`, or user-mintable state.

The internal `unsafe.memory` effect bit may remain as compiler metadata for
auditing trusted core/runtime operations. It does **not** authorize a guest
source escape hatch and must not turn `Pointer<T>`, `RawPointer<T>`,
`rt raw`, `rt ref`, `rt deref`, or `rt unref` into valid source.

Derived native views must retain the lifetime/provenance of their owning arena,
allocation, platform grant, or device window. Arena retirement, reallocating
mutation, actor teardown, hot reload and cancellation must fence outstanding
native leases. A naked integer never grants pointer authority.

## Backend-only lowering work

1. Keep raw/address representations in backend IR/runtime types that cannot be
   named by ordinary Oreslang source.
2. Bind every raw load/store/arithmetic/cast to compiler/runtime provenance,
   bounds, address-space, generation and authority checks.
3. Keep MMIO volatile/atomic access separately typed and platform-authorized.
4. Tie native leases to actor arena generations, teardown, hot reload and
   unwind/cancellation retirement.
5. Maintain source negative fixtures for all pointer-style spellings and assert
   rejection before IR/artifact publication.
6. Run ASan/UBSan/TSan and exact-tree tests; runtime-host tests alone do not
   prove guest compiler safety.

See the shared `docs/POINTER_CAPABILITY_CONTRACT.md` in
`oreslang-common-source` for the cross-backend contract.
