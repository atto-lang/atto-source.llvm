# Native AST static safety prototype (LLVM)

Status: experimental **subset-only** typed native AST helper. It is not a
source-level callback/Zalgo checker, a general taint/FFI security analyzer,
full termination proof, resource-limit proof, or production compiler gate.

`oreslang/types/StaticSafetyAnalysis.hpp` exposes
`analyze_native_static_safety(const Program&)`, returning conservative
findings with stable IDs and callable / SCC-member evidence.

Rules:
- `ORES-TERM-002`: cooperative scalar loop with no finite-bound proof.
- `ORES-AMP-004`: recursively reachable native direct calls detected by SCC.
- `ORES-AMP-001`: recursive chain including actor creation **and**
  provable message delivery to a locally spawned actor receiver. A spawn
  with no message must not be mistaken for handler execution.
- `ORES-ANALYSIS-001`: a send to an actor reference whose destination
  cannot be resolved from a local spawn; event graph is incomplete.

The existing native `RealtimeAnalysis.hpp` supplies call/effect summaries.
The analyzer does not replace hard-RT admission, ownership checks, compiler
lifetime verification, or host permission/sandbox enforcement. It does not
apply to Java-only AST forms or async/await syntax unsupported by the native
source parser. A future LLVM callback timing analyzer must integrate explicit
parser/HIR support; do not infer safety from missing callback data.

The prototype is a **library API** plus a native CTest target, not an added
ad-hoc CLI flag. The public CLI is being migrated to flags-2-env and shall
expose `ores analyze` only once verified compiler semantic facts are available.

Tests: `tests/native_static_safety_test.cpp`; CTest
`native_static_safety_ast_contract`. Run it with the exact candidate
commit and verify that CI executed its test steps. Missing runner allocation
or zero-step Actions failures are not a pass.

Backend/shared design:
https://github.com/ores-truffle-oreslang/oreslang-common-source/pull/106

Callback/borrow-check design:
https://github.com/ores-truffle-oreslang/oreslang-source.llvm/issues/743
