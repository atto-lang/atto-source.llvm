# Java Oreslang to LLVM port contract

Java source of truth: [oreslang-source.java](https://github.com/ores-truffle-oreslang/oreslang-source.java).
LLVM roadmap: [issue #1](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/issues/1).

## Compiler fragment currently accepted

- Zero or one `define module M [as] ... end` wrapper.
- Multiple zero- or multi-argument `[pub] fnc`/`routine` declarations,
  with `int` parameters and `: int` return annotation.
- Exactly one `return <expr>;` statement per function.
- Decimal int literals, unary negative, binary add/subtract/multiply, grouping,
  integer parameters, same-module function calls (including forward calls).
- Whitespace, `//` comments and nested `/* ... */` comments.

Semantic check runs before LLVM lowering: rejects unknown variables/callees,
mismatched call arity and duplicate declarations/parameters. Unexpected tokens fail closed.
The LLVM verifier runs before IR is emitted.

## Cross-implementation status

| Java capability | LLVM status |
| --- | --- |
| Lexer, modules, functions | Partial i64-only subset, not full Java parity |
| Function calls, expressions | i64 arithmetic and same-module calls |
| Type/ownership checking | Only parameter/name/arity checking |
| LLVM IR verification | Implemented |
| Compile and execute native guest | Clang can link i64 main on supported host |
| General target-specific ABI/object emission | Not implemented |
| Actors/supervisors, async frames, channels | Not implemented |
| Cycles/Ref/WeakRef, borrow checking, arenas | Not implemented |
| HPC/HFT and isolation | Not implemented |

**Unreconciled:** Java `int` width and overflow, possible runtime faults,
name mangling/linkage, operator semantics, hot reload and actor isolation. Arithmetic
currently lowers to LLVM signed i64 operations **without** `nsw`/checked traps; resulting
wraparound is provisional. Division deliberately remains unsupported to avoid undefined
zero-divisor and signed-overflow behavior. Native `main` process codes only test small
positive results.

### Tests

`ctest --test-dir build --output-on-failure` checks valid/invalid syntax,
LLVM IR fragments and, if Clang is available, native compilation + execution.
A workflow failure with zero allocated runner steps is not an executed test.
Record the exact Java source SHA in future differential fixtures, rather than claim parity.

Next priorities: imported Java lexer golden cases, unambiguous checked integer semantics,
structured control flow, typed return analysis, precise ownership and actor ABI,
before introducing scheduler or pinned-frame execution.
