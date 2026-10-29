# LLVM PR #1114 — CI acceptance and authorization handoff

**Status: BLOCKED / NOT AN EXPORT AUTHORIZATION.** This document is a source-free audit handoff on the draft PR branch, not a policy approval. No private Git tree or source code may be mirrored to another organization on the authority of this file. The immutable gates in `docs/security/CI_PRIVATE_SOURCE_MIRRORING.md` and the publisher remain controlling.

## Source and verified source-free test evidence

- Source: `ores-truffle-oreslang/oreslang-source.llvm`; draft PR #1114.
- Tested source revision before this documentation commit: `4ccf93d0bd8a297da91b2570580e305d77f192e7`.
- Locally executed the actual Python unittest entry point `tests/test_native_aggregate_negative_receipt.py` with standard Python and `python3 -O`: **63 of 63 PASS in each mode; 126 of 126 total**.
- Every required source-free file was independently compared byte-for-byte to its exact Git blob SHA:
  - `tests/native_aggregate_negative_receipt.py` — `4f33a7e08d87d7f246fbeb9bc3685913f8d7df88`
  - `tests/test_native_aggregate_negative_receipt.py` — `02f226924c1cd3f07d65e32cc11017af42a61f7a`
  - `tests/native_aggregate_kind_failclosed.py` — `1a88d570e344385b8c4a040f96ec753dbfaf453c`
  - `tests/native_aggregate_positive_execution.py` — `45b0cb0ec0ca647dc1f703696b13ad43708a077e`
  - `tests/test_native_aggregate_positive_execution_harness.py` — `2a1ccbccb24306a9b3d29b16666fe46b36985bfc`
- Python syntax compilation passed. No `oreslang-llvmc` binary, real 33-case compiler outcome, native guest executable, or exact-head compiler receipt was produced by these tests. Test-source hashes do **not** independently establish the trustworthiness of a compiler build.

## Live governance observations: every export gate still denies

1. Source `main` branch reported `protected=false`. Enforcement of CODEOWNER review, stale-review dismissal, administrator enforcement, no force push/deletion and no bypass actors is therefore not attested.
2. Protected-main destination allowlist `.github/private-ci-approved-repositories.txt` contains **zero** uncommented destinations.
3. Platform-conformance P0 incidents [#22](https://github.com/ores-truffle-oreslang/oreslang-platform-conformance/issues/22), [#77](https://github.com/ores-truffle-oreslang/oreslang-platform-conformance/issues/77), and [#127](https://github.com/ores-truffle-oreslang/oreslang-platform-conformance/issues/127) are all **OPEN**. Each must become exactly `closed` with `state_reason=completed` on independent incident-owner approval, not merely be marked resolved by a CI pass.
4. The reviewed protected-source carrier contract `.github/private-ci-carrier-contracts.json` contains only `opto-sync-test/go-java-validation` plans for PRs #669 and #674. **No approved contract/test plan for PR #1114 or candidate `honeypot-r-us-test/hnpt-lib-core` exists**.
5. Candidate PRIVATE carrier `honeypot-r-us-test/hnpt-lib-core`, repository ID `1343209023`, currently has unprotected `main` at `cce5fd525ae02ff2a30509acbc5e8427cb90f60b`. Its workflow `.github/workflows/oreslang-exact-tree-carrier.yml` is **not present on carrier main**; the successful source-free verifier executed on PR branch `security/source-free-carrier-integrity-20261028` at `55ee2007cba96a5014d4051d66deb1bd72b19970`, workflow blob `5719b139b84bd876eeb0b7057f12554921d4efcc` (carrier PR #19). This is **not an approved carrier-main pin**.
6. Source-free carrier Actions [run 40812515762 attempt 2](https://github.com/honeypot-r-us-test/hnpt-lib-core/actions/runs/40812515762), verifier job `123261953781`, received real runner `1000001540` and passed its source-free steps. This demonstrates execution capacity for that job only, **not remaining billing budget, source-transfer approval, or Oreslang compiler certification**.
7. Source LLVM exact-head Actions jobs still fail before recording any steps or runner assignment. Without their check annotation or administrative evidence, the billing-vs-policy root cause remains unverified.

## Independent owner actions, in required order

- Incident response/security: complete forensic inventories, access/audit review, sensitive-data assessment, GitHub Support purge scope and residual-risk review for all three P0 issues. Only incident owners may resolve/close them with evidence and `state_reason=completed`.
- Source repository administrator: enforce and independently verify `main` branch protection, strict required code-owner reviews, stale-review dismissal, no bypass, admin enforcement, and no force pushes/deletions. A CODEOWNERS file alone is insufficient.
- Carrier security owners: review PR #19 independently and merge the exact source-free workflow to carrier main through an approved path; either protect carrier main equivalently or produce the V3 reviewed-PR identity and exact-tree attestations required for an unprotected carrier. Confirm live numeric repo ID, privacy, current main SHA and workflow SHA after the review/merge.
- Protected-main source reviewers (including `@ORESoftware`): approve a **separate**, exact destination entry, numeric-ID-bound carrier contract, pinned reviewed carrier-main SHA, workflow blob, reviewed test-plan blob restricted to LLVM PR #1114 and exact commands. Do not accept a PR-branch workflow SHA as a carrier-main SHA. Enforce expiry/revocation, minimum permissions, no unreviewed source-bearing artifact/caches, independent provenance verification and cleanup/retention.
- **Only after all prior gates independently PASS**, a trusted protected-source V3 publisher may export the exact full PR tree to a verified private isolated `ci/oreslang-<SOURCE_ROOT_TREE_SHA>` ref, compare every Git blob and root tree SHA, create reviewed receipt/dispatcher marker, and dispatch the approved private carrier. No cross-org checkout secret is required inside the carrier; token-free transport does not bypass authorization.

## Exact-head native compiler acceptance required after approval

On the authorized exact commit, with a trusted LLVM/Clang toolchain:

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --target oreslang-llvmc oreslang-native-aggregate-kind-test
ctest --test-dir build --output-on-failure -R '^native_aggregate_'
```

Acceptance must record:
- Source repo numeric ID, PR #1114, exact HEAD SHA, full root Git tree SHA, independently attested test-plan and carrier identity.
- Exact compiler executable SHA-256, Clang executable digest and immutable toolchain identity, complete runner job steps and logs.
- **33/33** real compiler-negative cases with complete diagnostic/hash receipt and positive scalar control.
- **4/4** actual guest source → LLVM IR → Clang → native OS executions: exit codes `42, 19, 7, 73` in fixture order, with zero unexpected output/artifacts and guarded guest-status linkage.
- Sanitizer/ownership acceptance and a separate independent security review before moving draft PR #1114 toward merge.

**Prohibited shortcuts:** merging or adding an allowlist under unprotected main; manually setting incidents closed without forensic completion; direct connector blob/tree export without publisher authorization; exporting source to public/internal carrier; rerunning historical source-bearing forensic carrier refs; accepting source-free tests or a successful carrier canary as real compiler evidence.
