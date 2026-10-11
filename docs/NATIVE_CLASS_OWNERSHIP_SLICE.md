# Native class ownership: bounded i64 field slice

This compiler feature implements **source-level LLVM class fields and exclusive
borrowing in a deliberately restricted, non-escaping subset**. It is not full
Java/Graal class, Rust lifetime, or actor suspension parity.

## Accepted source

```ores
define class Box as
  pub let int value = 0;
end

fnc set(borrow mut Box target): void {
  target.value = 42;
  return;
}

fnc read(borrow Box target): int {
  return target.value;
}

pub fnc main(): int {
  let Box box = new Box();
  set(rt borrow mut box);
  return read(rt borrow box);
}
```

Every instance is a function-local, stack-allocated LLVM record containing
one i64 field initialized from the class declaration. Field loads and stores
operate on its address. No runtime-managed class allocation, object graph, or
collector is involved. Stack lifetimes end in reverse lexical order after
the final return value has been evaluated.

## Ownership rules

- A class is non-`Copy` by default; passing it by value is **rejected** until
  source-level move and destructor lowering exist.
- `borrow Box` can read its field but cannot write it; `borrow mut Box`
  can read and write the field during the callee's synchronous execution.
- Loan acquisition and release are tied to a synchronous call. Duplicate
  mutable loans, mutable plus shared loans on the same slot, mutable reborrow
  of an immutable formal parameter, temporary borrows and use-after-move
  must fail in semantic analysis, producing no LLVM IR.
- Reborrowing a `borrow mut Box` into a nested shared call is allowed while
  the nested call is active. The shared loan is released before the outer
  mutable borrow resumes.
- **No outstanding loan may cross `await`, `select`, actor hops or
  cancellation**. The compiler rejects these constructs rather than
  advertising unsound or incomplete native support.
- The only supported constructor is `new Box()`, only in a local `let`
  declaration. No constructor parameters, inheritance, escaping class
  references, pointer fields, multi-field layouts or methods are admitted.
- Native exception and panic unwind edges, general ownership drops,
  independent GC heaps per actor, and cross-file class imports remain
  unsupported.

## Conformance boundary

The exact shared fixture at
`contracts/v1/semantics/fixtures/ownership.ores` attempts
`change(Box box)` and `box.value = 1`. It is rejected with an explicit
`ownership violation` diagnostic, not a generic unknown-class parser error.
Matching one negative fixture does not prove full Oreslang parity.

The CTest matrix includes four Clang-compiled guest programs, seven
fail-closed source fixtures, plus a direct exact-shared-fixture test.
All source-level compiler checks execute before LLVM IR is emitted.
