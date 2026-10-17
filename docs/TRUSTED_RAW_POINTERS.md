# Trusted raw pointers for native / embedded Oreslang

Status: **native runtime foundation + fail-closed source gate**, 2026-10-17.

Oreslang's safe `Pointer<T>` remains an affine owning handle. It does not gain
numeric address semantics or pointer arithmetic. Native/embedded escape-hatch
work uses a distinct future source capability, `RawPointer<T>`, guarded by the
`unsafe.memory` effect and a trusted execution domain.

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
- Integer-created raw pointers reject null, alignment violations and arithmetic
  overflow. They are intentionally unbounded: memory validity remains the
  caller/target contract.
- Ordinary and volatile load/store paths are distinct so MMIO code is not
  accidentally compiled as non-volatile access.
- The source parser explicitly rejects `rt ref`, `rt deref`, `rt unref`
  and `rt raw` before IR until typed provenance/lifetime lowering exists.

## Trust and actor rules

A raw pointer is not admitted merely because code is native. Source lowering
must establish both:

1. the current execution domain is trusted (never an `UNTRUSTED` actor or
   sandboxed/portable guest), and
2. the enclosing callable's transitive effect row contains `unsafe.memory`.

The compiler/runtime may additionally require a native/embedded target profile.
Raw pointers are non-owning capabilities and are never actor-mailbox,
serialization, `Send`, or `Share` values by default.

A raw view derived from an owning pointer/arena does not erase its original
loan. Owner move, `rt unref`, arena retirement, reallocating mutation, actor
teardown and hot reload must remain blocked while descendant raw leases live.
Integer-fabricated addresses carry no safe provenance, cannot participate in
provenance-based pointer subtraction/ordering, and remain an explicitly unsafe
boundary even if an address is later reused by another object.

## Intended source semantics

The shared contract uses these semantic forms:

```ores
fnc checksum(Pointer<byte> owner, int n): int using unsafe.memory {
    const RawPointer<byte> p = rt raw owner;
    // p + i is element arithmetic; byte element => byte arithmetic.
    // rt deref(p + i) is a raw read in the admitted unsafe scope.
    ...
}
```

Mutable formation is `rt raw mut owner` and requires a distinct exclusive
ownership/loan proof in addition to trusted-domain admission and
`unsafe.memory`; the effect alone cannot upgrade a read-only raw capability.
`p.addr()` exposes a `uintptr` only inside `unsafe.memory`. A reviewed
`RawPointer<T>.from_addr(uintptr)` path is intended for FFI/MMIO/embedded
targets. Safe `Pointer<T>` itself never becomes an integer and never supports
`+`/`-`.

## Remaining lowering work before source admission

1. Add a typed native IR/HIR representation for `Pointer<T>`,
   `RawPointer<T>`, provenance and raw-loan identity.
2. Carry actor trust kind into callable validation and reject raw operations
   under `UNTRUSTED` before code generation.
3. Require `unsafe.memory` transitively for raw formation, arithmetic,
   address exposure, cast and dereference.
4. Lower bounded arithmetic to checked GEP/address calculations with element
   scaling, address-space preservation and explicit one-past rules.
5. Lower raw read/write with alignment, bounds/generation checks when
   provenance exists; keep MMIO volatile/atomic operations separately typed.
6. Tie raw leases to actor arena generations, hot-reload retirement and
   cancellation/unwind drop plans.
7. Add guest-language positive fixtures only after the above exist; retain
   current negative no-IR fixtures until then.
8. Run ASan/UBSan/TSan and exact-tree tests. Runtime-host tests alone do not
   prove guest compiler safety.

See the shared `docs/POINTER_CAPABILITY_CONTRACT.md` in
`oreslang-common-source` for the cross-backend contract.
