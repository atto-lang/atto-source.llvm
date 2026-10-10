# LLVM ownership admission

The only native source type accepted today is `int` (lowered provisionally
as LLVM i64). `OwnershipChecker.cpp` is a real flow-sensitive pre-lowering
pass for this restricted type: it checks explicit `rt take` invalidation,
source-order reads, and ordered function-call argument evaluation.

- `rt copy expression` and `rt share expression` are valid only for the
  compiler's already-validated trivially copyable i64 expressions.
- `rt take x` evaluates and invalidates named scalar parameter `x`.
  Re-reading it inside the same expression or a later call argument fails.
- A temporary computed value (`rt take (x + 1)`) moves only that temporary.
- Borrowed parameters, `rt borrow`, `rt borrow mut`, reference projection,
  actor-boundaries, async/await, select/nb select/do nb select, readch and
  do/nb channel operations remain *unsupported and fail closed*, with no IR.

This does **not** implement Rust ownership for move-only structs, heap
objects, actor-owned arenas, cross-actor transfer or JIT/native frames.
In particular, no `rt share` of mutable objects or shallow aliasing is
allowed. Those features require equivalent Java type/borrow rules and
runtime lowering before native compiler admission.

Java authorities: `TypeChecker` and `OwnershipChecker`.
The native CTest matrix checks both valid primitive ownership IR/native
execution and negative borrow/actor/suspension rejection.

See [source parity](../../../../../../docs/SOURCE_PARITY.md).
