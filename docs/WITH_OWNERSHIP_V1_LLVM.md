# LLVM `with` admission: native fail-closed stage

`Parser.cpp` now detects `with {field => value}` at the actual lexer-token
boundary, including nested comments, before executable LLVM IR generation.
It does **not** reserve the ordinary identifier `with` or substrings
such as `withered`. Compiler-owned diagnostic:
`WITH_BACKEND_UNSUPPORTED`.

This is a **safety/admission parity milestone only**. It is not `with`
execution parity with Java/Truffle PR #2832. The native backend lacks
verified readonly record update lowering, class `rt copy self` construction,
zero-GC field-wise ownership and drop-once exception plans.

Four executable regression fixtures are included. The shell gate
`tests/test_with_ownership_v1.sh /path/to/oreslang-llvmc` requires an
**actual built compiler** and checks zero stdout IR on rejected update
sources, inert comments and harmless lookalike identifiers. This gate
must be executed on the exact source PR SHA under reviewed CI; its
presence alone is not a passing test.

Canonical 10-case positive/negative source contract:
`oreslang-platform-conformance` draft PR #693. The LLVM backend must
eventually compile the unchanged positive `.atto` guest programs under
O0/O2, execute real native results, and pass ASan/UBSan + ownership
drop counters before claiming implementation parity.

No private source has been transferred across organizations in this PR.
