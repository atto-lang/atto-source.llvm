# REVIEW ONLY — LLVM PR #1747 private-CI capacity and authorization plan

**Status: DRAFT / NO SOURCE EXPORT AUTHORIZATION.** This is a protected-governance review input. Neither this document nor raising the V3 source file-count cap authorizes a carrier, adds a test plan to the active contract, changes branch protection, waives an incident gate, or permits a source-bearing transfer. No carrier refs are created by this change.

## Immutable source metadata, read without moving bytes to another organization

- Source repository: private `ores-truffle-oreslang/oreslang-source.llvm`, repo ID `1410897874`
- Target: stacked LLVM PR [#1747](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/1747), exact head `1af9778b1ef97babba5f6810fd828511535845aa`
- Actual source root tree: `2689aa1d3168b3ef9e860e19133ea6c4d3599e5e`
- Git tree metadata: `truncated=false`, **1,920** entries = **1,829** regular blobs + **91** trees, total declared blob bytes **9,437,381**, largest blob **363,431** bytes, zero unsupported symlink/submodule file modes
- Existing V3 publisher caps: `MAX_FILES=1024`, `MAX_TREE_ENTRIES=4096`, `MAX_TOTAL=40 MiB`, `MAX_FILE=10 MiB`
- Thus the exact PR tree would be rejected **before blob transfer** by the old file-count cap; its size and individual blobs fit current other bounds
- Proposed cap: `MAX_FILES=2048`; keep the remaining caps unchanged. The capacity increase requires explicit independent security review of resource/time/API-rate and retention risks.
- **Stacked-PR blocker:** #1747 currently targets feature branch `feat/shared-immutable-payload-host-20261112` (LLVM PR #1742). V3's `verify_source_pr` strictly requires an open PR whose base is `main`; a full 1,829-blob export is still denied even with a raised cap. Do not weaken this check. Options for independent review: merge #1742 after its own gates then retarget #1747 to main, or create a single reviewed combined native-scheduler PR against main. Refresh all immutable pins after any retarget/rebase.
- Evidence labels: source tree **READ_ONLY_METADATA_VERIFIED**; publisher capacity tests **AUTHORED_NOT_EXECUTED**; source-export **NOT_AUTHORIZED**; LLVM host/guest **NOT_EXECUTED**.

## Security gates currently unsatisfied

1. Required issues in [oreslang-platform-conformance](https://github.com/ores-truffle-oreslang/oreslang-platform-conformance): #22, #77, #127, #212 and #222 remain **open**. Retain forensic refs; incident owner must independently investigate/remediate and close each as `completed`, with immutable `closed_by=ORESoftware` identity, before any V3 publication.
2. Source `main` at `6539621436f1614a789d75a7a3f4b8004ecedd7c` reports `protected=false`. Existing tracking issue [LLVM #915](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/issues/915) explains the private-repo protection entitlement and administrative restrictions. No connector action in this session supports imposing branch protection. Require effective live CODEOWNER + independent review, stale approval dismissal, admin enforcement, and no bypass/force push/deletion. Retest `protected=true` plus full protection details before export.
3. The approved destination allowlist currently names only **private** `streempilot-test/browser-studio-e2e` (repository ID `1323599136`), but the active carrier contract contains only its LLVM #787 plan; it does **not** authorize #1747. Its live `main` head `0fd571f333009bb3365ae9932f2ee9fc464f5b45` differs from contracted reviewed base `bc587586dbc5e8a12f86677b6f044ec0c5a2b66d`, and live carrier `main.protected=false`. Fresh immutable reviewed carrier base/workflow, independent signed review/alternative reviewed-main identity, per-PR plan and TTL/retention are required.
4. Previously attempted opto approval [LLVM #715](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/715) is **closed unmerged**; `opto-sync-test/go-java-validation` is **not** in the active LLVM allowlist and cannot be used merely because its Actions minutes work.
5. The V3 ref-namespace cleanup/ABA risk is separately tracked in [LLVM #1154](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/issues/1154), with draft [PR #1146](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/1146). Require security owner disposition of this fail-closed cleanup issue; a capacity increase cannot fix it.
6. Carrier transfer must use the approved protected-source V3 publisher and exact receipt, **not direct connector blob/tree writes**. Full source tree SHA, all blob SHA/modes, source repo numeric ID, destination repo numeric ID, current approved base/workflow/test-plan blobs, receipt expiry and cleanup require independent verification. No generic cross-org `GITHUB_TOKEN` checkout.

## Review decision on runnable test plan, after authorization

The existing V3 `ores-private-ci-test-plan-v1` schema permits only allowlisted executables (`python3`, `mvn`, `cmake`, `ctest`, `g++`, `clang++`, `/tmp/ores-*`); arbitrary `bash`/shell is rejected by `validate_test_plan`. Existing #1747 `tests/ci/verify_shared_payload_real_scheduler.sh` **cannot simply be placed in the reviewed plan**. A separately approved carrier plan must use the actual trusted CMake/CTest path (or reviewed, whitelisted runner extension) with exact compiler selection, genuine stepful GCC/Clang × O1/O2, ASan+UBSan and a nonvacuous test marker. Do not loosen the executable allowlist to accommodate a PR-supplied shell command.

Proposed test selection (only after actual carrier approvals): native source `NativeActorScheduler.c`, `NativeActorSharedPayloadBridge.hpp`, `SharedImmutablePayload.hpp`, and `native_actor_shared_payload_real_scheduler` CTest. Require 100 legitimate 1 MiB descriptor deliveries, wrong-mail ticket rejection, CONTROL-before-DATA, negative actor-ref receipt, 100 post-delivery replay denials, and STOP_SEALED cleanup of 100 descriptors (101 queued DATA envelopes including malicious-host probe).

## Source-free synthetic capacity regressions added on this PR

`scripts/test-private-ci-v3-source-tree-capacity.py` calls the actual V3 `exact_source_tree` with mocked, synthetic GitHub API responses only:

- 1,829 files under the proposed 2,048 cap: accepted and each synthetic blob hash revalidated
- 2,048 files at boundary: accepted; 2,049 files: denied before any blob fetch
- 1,829 × 24,000-byte declared files: aggregate 40 MiB cap denies **before** any blob fetch
- Stacked PR not based on `main`: denied before tree read

**No source export, actual test execution or positive approval is claimed.** Keep this PR draft; security reviewers must decide on safe cap/publisher behavior and execution cost before any merge.
