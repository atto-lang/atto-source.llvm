# NIO / Actor Native LLVM Reconciliation Candidate

**Status: DRAFT — code merged in Git objects, not compiled or runtime-certified.**

## Immutable input identities
- Existing integration: [#638](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/638), exact commit `879341a70c79d4ef333327a72046e53e1d400ebd`, tree `c51c56a882471838de8d796c8ba68f1f17082a81`.
- Source `main` commit `d97094252860dc9b952cfb42e18afba9a872b666`, tree `a8e108cf1f2a56b16f5670d787d8d29e54b72ec8`.
- Git merge base `d9c2f1c4c52fd70e96d3b41471786869199fcdf0`.
- This tree was assembled **in the originating private GitHub repository** using Git tree and blob identifiers. No source or Git objects were copied to another organization.

## Tree reconciliation witness
- Exact **39 feature-only file blobs** preserved from the refreshed pinned integration head, without content changes. Two workflow/security-fixture blobs were refreshed after initial tree assembly (source head advanced by two commits).
- Exact **29 upstream-only file blobs** preserved from the pinned `main`, without content changes.
- Six files are genuinely combined instead of choosing one side wholesale:
  1. `CMakeLists.txt`: retain the upstream CPU/actor/DOP tests plus integration NIO tests.
  2. `src/main/cpp/oreslang/frontend/Parser.cpp`: preserve integration actor-thread annotation parsing; choose the stricter upstream V3 `?`/`@DefaultArgVals` lexer diagnostics (which still admits `@` tokens).
  3. `src/main/cpp/oreslang/runtime/NativeActorScheduler.c`: preserve upstream physical turn gate and feature RT/actor turn admission changes.
  4. `src/main/cpp/oreslang/runtime/NativeActorScheduler.h`: retain both sets of API declarations.
  5. `src/main/cpp/oreslang/runtime/NativeOresVM.c`: retain upstream CPU physical budgeting and integration bootstrap / owned NIO lease lifecycle. The distinct API function blocks inserted at the same base line were concatenated in that order.
  6. `src/main/cpp/oreslang/runtime/NativeOresVM.h`: preserve both upstream CPU/turn APIs and integration RT/NIO APIs; similarly concatenate independent declarations.
- The pre-commit Git-tree verification confirmed all 68 unshared paths have the expected original blob SHAs, all six combined blobs differ from both parents, and no unresolved textual edit overlap remains.
- **Do not mistake an automatic line merge or GitHub mergeable result for semantic conformance.** The combined C/C++ sources have not yet been compiled or reviewed in a full-tree build.

## Required next actions
1. Independent audit for double initialization, actor physical-turn admission order, CONTROL lane serialization, NIO `close(EINTR)` and unexpected-close quarantine, lease ownership, reactor stop, and cancellation terminal ordering.
2. Restore a **source-local approved runner**. Verify exact merged commit/tree plus actual runner identity, nonzero executed BUILD/TEST steps, logs and immutable evidence.
3. Run full CMake/CTest, LLVM compiler/guest runtime, native GCC/Clang ASan+UBSan and supported TSan; run actor/NIO boundary, parser, option/defaults and new CPU budget tests.
4. Only promote from DRAFT after passing complete source tests and independent approval. Do not interpret source-free synthetic tests as production certification.
5. Private cross-org mirroring is still prohibited while policy PRs #1123/#1217/#1300/#1317 lack independent approval and platform-conformance P0 #22/#77 remain open. Funded GitHub Actions minutes and a destination `GITHUB_TOKEN` alone are insufficient authorization.
