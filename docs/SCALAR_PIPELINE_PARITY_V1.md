# Native LLVM / Java value pipeline parity v1

Status: **AUTHORED_NOT_EXECUTED / NOT_CERTIFIED**. Narrow scalar compiler
slice based on LLVM main `0ee037ca3e5dd9ac6ac1e9cc3d40f19ee6326a9c`.

- `value |> f` and `value |> f(extra: argument)` desugar to ordinary Call
  AST with value inserted once as the first positional argument. Existing
  binding, scalar typing, generic, effect and ownership checks remain authoritative.
- LLVM v1 caps at 64 stages (existing native expression-depth budget);
  Java's frontend admits 128 stages, so full behavior parity is not certified.
- Infix actor `pipe` requires role typing, bounded edge transport,
  supervisor exclusion and lifetime safety; it remains explicitly rejected.
- No dynamic callable/lambda/member lifting, sum constructor transformation,
  Option/Result receiver combinators, or implicit Future flattening is enabled.

CMake fixtures test native IR, exit-42 compiled guests (simple and named stages),
and compile-negative arity/type/noncallable/budget/actor syntax.
Tests need exact-source compiler builds, real CI steps, O0/O2 native execution,
and independent provenance/IR/artifact verification before any promotion.
A runner with zero executed steps is NOT_EXECUTED, not a semantic failure.
No Native Image, whole-backend, or zero-GC parity claim is made.
