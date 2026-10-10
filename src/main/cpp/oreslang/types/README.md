# LLVM ownership admission

The LLVM native compiler has a deliberately narrow i64-only frontend. Its
flow-sensitive ownership checker runs **before** IR construction. This is not
the full Java/Graal ownership model.

## Implemented scalar ownership and scoped loans

- Primitive `rt copy`, `rt share` and `rt take` preserve source-order
  value semantics; explicit takes invalidate named owned i64 parameters.
- `borrow int x` and `borrow mut int x` are typed function parameters.
  They pass an i64 pointer to a caller-owned stack slot, not an i64 value.
  Callees can read their value as `x`, but cannot take their owner.
- `rt borrow x` and `rt borrow mut x` are admitted **only** as arguments
  to matching borrowed parameters. The source must be an owned, named i64
  parameter. Borrowing integer temporaries or borrowed parameters again is
  intentionally rejected.
- Immutable loans can coexist; mutable loans are exclusive. A mutable loan
  forbids reads or moves of the owner during call-argument evaluation. A
  shared loan forbids taking its owner until the call returns.
- Both the checker and LLVM emitter evaluate arguments left-to-right, and
  a nested call observes all loans still held by its enclosing call.
  Loans release immediately after their direct call returns.
- Every native function returns i64. Borrowed references cannot be stored,
  returned, captured, placed in a field or retained past their call.

Example supported source:

```oreslang
fnc inspect(borrow int x): int { return x + 1; }
fnc wrapper(int x): int { return inspect(rt borrow x) + rt take x; }
pub fnc main(): int { return wrapper(20); }
```

`borrow mut` is currently an exclusive read-access capability only: this
fragment has no assignment syntax, no native mutable struct field projection,
and no mutable reference writes. Its ABI is deliberately not certified as a
stable external C/FFI interface.

## Not supported

Actor-owned object graphs; structs; heap ownership; destructor/drop timing;
mutable projections; reborrows; reference-valued variables or returns;
pinned frames; asynchronous suspend/resume; cancellation; `await`, `select`,
`nb select`, `do nb select`, `readch`, `do readch`, `do nb readch`,
and actor migration all still **fail closed without LLVM IR**.

No claim of Rust-like zero-GC object lifetimes, actor isolation, Java parity
or suspension safety follows from the scalar-only ABI. Every new reference
kind needs its own alias/provenance, escape, drop, actor transfer and
suspension rules, with executable positive and negative tests.

Java authority: `TypeChecker` / `OwnershipChecker` in
`oreslang-source.java`. See [native/source parity](../../../../../../docs/SOURCE_PARITY.md).
