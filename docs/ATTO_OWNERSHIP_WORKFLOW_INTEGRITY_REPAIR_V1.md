# AttoLang ownership CI workflow corruption hardening

**Source-only workflow repair; non-promoting. LLVM production compiler, C++17 CTests, 50 .atto compiler/Clang fixtures and Java guest tests remain NOT_EXECUTED until runner admission is restored.**

## Finding

The current LLVM typed-MIR ownership draft #1526 had a duplicated and malformed YAML tail in `.github/workflows/atto-critical-ownership-llvm-parity.yml`. After the intended final exact-head/tree check, an unindented stray `/tmp/atto-place-inventory.txt` line and an incomplete `grep` expression introduced a second copy of several compiler/test steps and a stale `9`-CTest count. This is a source/workflow defect **independent of** the GitHub Actions billing/payment-or-spending-limit admission failure.

## Minimal repair

- Removed only the appended malformed tail. The canonical YAML now ends at the single `ATTO_LLVM_FINAL_TREE` verification, retaining the nine authored named compiler steps, checkout action pinned to its SHA, eleven focused native CTests, twenty scoped compiler CTests, and 50-program real compiler matrix.
- Added `scripts/ownership-ci-workflow-shape.mjs` (pure fail-closed static structural checker), `bin/check-ownership-workflow-shape.mjs` (CLI), and `tests/ownership_ci_workflow_shape_14.test.mjs`.
- The checkout/head binding step now runs `node --check`, the 14-case test suite with exact TAP test/pass/fail counts, and the static checker **before** Clang/LLVM installation. The workflow paths include all three new files.
- The validator detects missing/reordered/duplicated named steps, adjacent duplicated jobs, stray script commands, trailing material after the final receipt, stale CTest cardinality, missing required case/inventory checks, unpinned checkout, missing compiler matrix, and invalid source bytes.
- The validator is intentionally strict for this workflow version: any legitimate workflow refactor requires a reviewed version of the structural assertions.

## Evidence and bounds

14/14 isolated-V8 execution of fetched exact validator + test text with a minimal Node assertion shim; the current fetched cleaned YAML passes the static validator. **This is not hosted Node/Actions, not a production C++ build, and not a compiler-backed .atto guest receipt.**

The validator returns `structurallyValid=true` only for its expected source shape, and always `verified=false`, `guestExecutionVerified=false`. The CLI prints `ATTO_OWNERSHIP_WORKFLOW_SHAPE=PASS_STATIC_ONLY` and `ATTO_OWNERSHIP_GUEST_PARITY=NOT_EXECUTED`. A workflow inventory or test count cannot substitute for actual executed steps or JUnit/guest receipts.

## Release gate

The precise GitHub check annotation recorded in conformance infrastructure issue #315 says the job was not started because recent account payments failed **or** the account spending limit must be increased; only the authorized billing owner can distinguish/resolve the two conditions. After approval and recovery: execute a same-org no-checkout source-free canary with nonzero runner/job steps; run the exact immutable child head/tree through the 14 static tests, eleven focused native CTests, twenty scoped compiler checks and fifty distinct real LLVM/Clang guest programs. Require independent ORESoftware approval and do not merge based on `mergeable=true`. No external org source mirroring or billing modification was performed.
