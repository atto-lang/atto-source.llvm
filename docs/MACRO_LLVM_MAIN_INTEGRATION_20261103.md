# LLVM `define macro` current-main integration / parity audit

**Evidence class: source-level forward-port only; native compiler/CTest/Clang
guest execution is NOT_EXECUTED. This is a DRAFT integration.**

## Provenance

- Forward-port target: **current** `oreslang-source.llvm/main`, not a forced
  merge of the historic macro draft.
- Source: [LLVM draft PR #1026](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/1026).
- Every patch hunk to the production `Parser.cpp` (**10**) and
  `CMakeLists.txt` (**1**) matched an unambiguous existing context on current
  `main`. The compiler/runtime changes made since the older base were
  preserved; old whole files were NOT copied over main.
- The other 14 new test/fixture files were copied byte-for-byte from the
  original macro draft.
- The seven stage-0 `.atto` fixture sources match
  [common-source #218](https://github.com/ores-truffle-oreslang/oreslang-common-source/pull/218)
  and Java [#2504](https://github.com/ores-truffle-oreslang/oreslang-source.java/pull/2504)
  and .NET [#346](https://github.com/ores-truffle-oreslang/oreslang-source.net/pull/346).

## Shared portable scalar stage-0 semantics

- Two compiler-only forms:
  `Const<int> -> Syntax<Expr>` using exact `quote(value)`, and
  `Syntax<Expr> -> Syntax<Expr>` with a single structural AST identity splice.
- Signed i64 addition/subtraction/multiplication are checked during constant
  expansion. Unclosed constants, wrong argument kinds, duplicate/unresolved
  macros, reserved `quote`/`unquote` names, unsupported declaration results
  and multi-parameter bodies reject before code generation.
- Whole-translation-unit limit **8 declarations**; **128 expansions**; max
  **512 constant AST visits**, evaluator depth **64 inclusive**.
- Scoped macro-input parser slack supports 64 AST edges plus the enclosing
  invocation grammar, without changing the ordinary expression depth limit.
  The evaluator itself rejects constant depth 65. The parser scope uses RAII
  to restore the untrusted-source depth budget on success or exception.
- Test corpus adds positive 511-node and negative 1023-node const trees,
  8/9 declarations, 128/129 expansions and const depth 64/65 cases.
- The production compiler's native guest test generates LLVM IR, invokes
  Clang, executes the linked binary, validates exit 42 with empty stdout and
  stderr, rejects stale binaries, and records SHA-256 of source/compiler/IR/
  executable on success. No macro runtime/JIT fallback.

## Important non-parity still open

- Java supports richer `define macro` semantics including typed syntax
  templates, identifier hygiene/capture and more const types; this LLVM
  implementation only covers the **closed scalar stage-0 intersection**.
- The broader Java/.NET/Wasm/Forge macro feature matrix is **not certified**.
- Successful fixture blob hashes and compiler AST assertions are **not**
  evidence of an executed LLVM/Graal/.NET native guest. No meaningful CI
  result exists while source-org jobs fail before obtaining runners.
- Before merge: actual exact-head CMake compile, full CTest, Clang guest
  execution, negative no-IR tests, sanitizer runs, fresh mainline integration,
  independent security/code review and source-owned runner admission.

**Never mirror this private compiler tree to another organization without
independently approved exact source-export policy, regardless of runner
minutes.**
