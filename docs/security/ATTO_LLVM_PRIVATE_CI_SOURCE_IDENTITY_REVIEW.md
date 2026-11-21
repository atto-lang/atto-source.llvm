# Private LLVM CI source identity migration — review only

## Scope and source binding

- Publisher source repository: atto-lang/atto-source.llvm.
- Exact source repository ID: 1448700962 (verified with connected GitHub repository metadata).
- Historical identity ores-truffle-oreslang/oreslang-source.llvm / 1410897874 is archived; do not use it for new source PR verification.
- Source branch remains main. This draft changes ONLY the two identity constants and source-free regression test.
- Legacy Oreslang platform-conformance incident gates, reviewer identities, existing carrier allowlist and test-plan SHA pins remain unchanged. They require independent review/migration before any operation.

## Unconditional export denial remains in place

Production publish() starts with require_atomic_ref_rollback(), which raises Denied unconditionally for unresolved atomic-ref cleanup issue #1154. Source identity and a working destination GITHUB_TOKEN cannot override this. The new test asserts that the migrated identity stays bound and Denied occurs before GitHub API or bounded-gh calls.

## What would be required to activate authorized token-free CI

1. Independent security administrator resolves atomic ref cleanup ABA and protected branch requirements; do not weaken guard for convenience.
2. Independently review any incident migrations, exact destination repository ID, allowlist, workflow blob, test plan SHA and PR number(s). Current carried plans cover only historical PR numbers and do not authorize #834.
3. Establish actual runner capacity with source-free jobs, then mirror only reviewed exact full Git tree to the approved private carrier and verify per-blob SHA, modes and source provenance; no private export until the above are satisfied.
4. Execute CMake/CTest, native guest O0/O2, negative no-IR gates and sanitizers, and record nonzero job steps and exact head hashes. Zero-step action failures do not count as code failures or passes.

**This change does not authorize source export, create carrier branches, dispatch CI, or unblock merges. Tests not yet executed. Keep draft.**
