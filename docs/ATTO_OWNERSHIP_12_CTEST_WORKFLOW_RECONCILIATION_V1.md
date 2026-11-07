# AttoLang ownership workflow — 12-CTest stacked reconciliation

**November 7, 2026 — non-promoting, source-only repair.**

## Evidence and defect

LLVM typed-MIR ownership PR [#1529](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/1529) is stacked on #1526 but its own `.github/workflows/atto-critical-ownership-llvm-parity.yml` contained **1,024 lines**, eight orphaned unindented `/tmp/atto-place-inventory.txt` fragments, a truncated MIR inventory `grep`, repeated final head/tree checks and duplicate compiler fixture steps. It also contained stale eleven-CTest assertions alongside the intended twelve-CTest JUnit proof.

A previous sibling repair [#1528](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/1528) addressed the earlier eleven-CTest ancestor. Copying its whole workflow over #1529 would DROP the twelfth opaque-effects native test. This child therefore preserves the new #1529 semantics and applies the corruption fix specifically to its immutable tree.

## Code changes

- Truncate only the duplicated malformed suffix *after* the first canonical final source head/tree receipt.
- Restore **both** mandatory inventory `grep` assertions for `native_ownership_mir_receipts_30` and `native_ownership_mir_opaque_effects_18`.
- Preserve the 12 native CTest names, twelve-case distinct JUnit testcase verification, twenty scoped compiler CTests, the real fifty-source `.atto` compiler/Clang gate, pinned checkout SHA, and exact head/tree readback.
- Bring across the fail-closed **source-only** workflow-shape validator from #1528, adapted to the twelve-case inventory, plus 18 adversarial tests and static-only CLI.
- Run `node --check`, exactly 18 Node TAP cases and the static shape CLI before installing native compiler toolchains in the source-org workflow. File paths and branch pattern trigger PR and push workflows.

## Scope of the test result

The fetched exact module and suite passed **18/18** tests in isolated V8 with a lightweight `node:test`/`node:assert` shim. The actual Node 22 subprocess, source-org runner, production C++ compiler, CTests and 50 Clang-linked guests are **NOT_EXECUTED** at this stage. Successful static checks have `verified=false`, `guestExecutionVerified=false` and explicitly print `NOT_EXECUTED`; they never imply a validated compiler backend.

## Gate before review or merge

A literal GitHub annotation recorded under [conformance issue #315](https://github.com/ores-truffle-oreslang/oreslang-platform-conformance/issues/315) attributes source-org job non-admission to failed recent account payments **or** an insufficient spending limit. Only the authorized billing owner can resolve the exact alternative. After recovery, prove an allocated private source-org runner executes a source-free canary, then run the exact PR head/tree through hosted Node tests, all 12 non-skipped native ownership CTests, the 20 scoped checks, and all fifty real compiler/Clang guests. Actual SSA completeness, emitted field stores, Java/LLVM/.NET/Wasm/Forge guest parity and ORESoftware approval remain independent prerequisites. Do not export private source to another organization or relax review/security protections.
