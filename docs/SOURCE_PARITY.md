# Java Oreslang to LLVM porting contract

Source of truth: [oreslang-source.java](https://github.com/ores-truffle-oreslang/oreslang-source.java), `main`.
Native/HPC roadmap: [LLVM issue #1](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/issues/1).

## Directories / mapping

| Java source organization | Initial LLVM counterpart | Status |
| --- | --- | --- |
| `parser/Lexer.java` and `parser/Parser.java` | `src/main/cpp/oreslang/frontend/Parser.cpp` | Tokenizer + deliberately minimal grammar |
| `ast/Ast.java` | `frontend/Parser.hpp::ConstantFunction` | One typed AST node only |
| `types/TypeChecker.java`, `OwnershipChecker.java` | `src/main/cpp/oreslang/types/` | Future; **nothing admitted** beyond trivial literal function |
| `nodes/OresEvalRootNode.java` / JVM backend | `src/main/cpp/oreslang/backend/LlvmCodegen.cpp` | Basic LLVM IRBuilder constant-return function with verifier |
| `runtime/ActorRuntime.java` and scheduler/Future | `src/main/cpp/oreslang/runtime/` | Not implemented |
| `stdlib/`, `contracts/`, `examples/` | `tests/fixtures/` | Golden parity fixture beginnings; incomplete |

## Bootstrap language fragment

Accepted:
- optional outer `define module IDENT as ... end`;
- optional `pub`;
- one top-level `fnc` or `routine`, zero arguments, integer return type;
- one body statement `return DECIMAL_LITERAL;`;
- line comments and whitespace.

Example:

```oreslang
define module demo as
  pub fnc main(): int {
    return 7;
  }
end
```

Everything else fails closed. `int` currently lowers to `i64` for this scaffold,
but the actual Java type width/overflow ABI has **not been reconciled**.
Unsupported actors, async/await, channels, classes, imports, structured
recursive aliases and `Ref/WeakRef` are NOT parsed or executable.
No claim of guest compatibility or a native no-GC hot path.

## Next parity steps

1. Capture the Java lexer/token fixtures and parser AST diagnostics from
   the exact pinned Java `main` commit. Add checked-in shared tests and
   compare both implementations without running untrusted Oreslang code.
2. Reconcile integer widths, signedness, overflow, String/UTF-8, tuples,
   generic types, exceptions and symbol ABI.
3. Implement module/function declarations, ownership validation and
   compile-negative parity before allowing arbitrary source-to-IR lowering.
4. Define the actor ABI, mailbox isolation and frame pinning models
   described in issue #1 before any native thread/core/GC controls.
5. Test in a real LLVM + Clang environment; a zero-step GitHub workflow
   is not evidence of a passing build. CI and CTest are required.

## Build

Requires CMake >=3.20, LLVM development package >=14, C++17 compiler.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
./build/oreslang-llvmc tests/fixtures/constant-main.ores
```

The binary currently prints verified LLVM IR to stdout. It does not link
or execute a guest program, provide a service API or parse CLI option flags.
