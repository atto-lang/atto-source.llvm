# Guarded mutable sharing: `rt mut borrow` and `rt mut share`

Status: Java/Graal JVM-backed `SharedMutex<T>`-only slice; LLVM/native fails closed.
This document is a compiler admission contract, **not** a general actor-heap sharing
or allocation-domain transfer implementation.

## Semantics

- `rt borrow x` creates a scoped read-only loan; no allocation or object copy.
- `rt mut borrow x` creates an exclusive scoped mutable loan. Borrow conflicts,
  owner capability, escape, cancellation and suspension rules still apply.
- `rt share x` shares only values already approved by the existing copyable
  handle rules. It does not turn move-only object graphs into aliases.
- `rt mut share x` retains a **builtin `SharedMutex<T>` handle only**.
  This operation neither clones `T` nor yields a `borrow mut T`.
  It returns the *same synchronized handle* with its existing lock/guard API.
  Every mutable access to `T` must acquire an exclusive guard. Plain `Foo`,
  `Mutex<T>`, primitives, borrowed `SharedMutex<T>` views and unresolved
  generic capabilities must be rejected.

The underlying `SharedMutex.new(ownedValue)` establishes the shared-memory
domain and performs existing shared-safe type checks. Reusing the handle does
not authorize importing actor-private pointers, crossing a tenant boundary,
or serializing live mutable graphs into a mailbox. A shared allocation is not
a copy and is not both a stack-owned object and a heap-owned object.

`val` freezes the name, not necessarily the referent: legal field mutations
remain subject to binding permissions, exclusive borrows and guards. Mutable
sharing does not turn immutable primitive values into writable cells.

## Compilation

The AST distinguishes `mut_share` from ordinary `share` and `&mut`.
The type checker requires precisely the nominal builtin
`SharedMutex<T>` result; the ownership checker additionally denies shadowing
nominal class/type aliases, non-copy handles and borrowed values. The JVM
evaluator verifies an actual runtime `OresMutex.Shared` before returning the
same object. The existing lock/guard machinery owns synchronization and release.

No implicit conversion, `rt copy`, raw `&` or `*` operation is introduced.
No new actor message transport is implied. An explicit authorization for
shared-memory capability remains mandatory under the runtime's isolate policy.

## Compiler tests

Java: `MutableShareCompilerTest` verifies distinct AST forms, shared-handle
runtime identity via protected writes, exclusive borrow conflicts, invalid
scalars/objects/local mutexes/borrows/generics, forged nominals, and malformed
spellings. These tests must run under Maven/JUnit; source-only assertions
do not establish execution.

LLVM: `native_borrow_admission` contains 12 `rt mut share` compiler-negative
contexts (20 native borrow denials total) and four real-IR positive controls,
including a canonical `rt mut borrow` control. Negative cases demand the
dedicated `rt mut share` diagnostic, **zero emitted IR**, no artifacts inside
or beside the scratch fixture, and a normal nonzero compiler exit (not a crash).
The Rust harness also has 14 source-free adversarial/predicate unit tests.
These additions require exact-head execution; earlier successful CI runs
do not verify later commits. Native guarded `SharedMutex<T>` lowering remains
unimplemented; a parser rejection is *not* native shared-mutable support.

## Future admission gates

1. Shared native `SharedMutex<T>` storage/ABI, lifetime and drop ownership,
   memory-order and fairness proof, and guard-only write paths.
2. Complete borrow/guard liveness over `await`, exceptions, cancellation,
   actor turnover, hot reload and runtime suspension.
3. Cross-isolate/tenant boundary policy: ordinary actor heaps remain isolated;
   shared arenas need explicit runtime-minted cross-domain authority.
4. Matching positive/negative Java, LLVM, .NET, WASM and Forge guest fixtures,
   with executed exact-head native evidence before reporting backend parity.
