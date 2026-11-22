# REVIEW ONLY — LLVM PR #1728 full Git-tree CI on an alternate private runner

**Status: NOT APPROVED — no additional private-source export is authorized by this document.** Do not interpret a green subset carrier test or an allowlist entry alone as a reviewed full-tree publishing contract. Source transfers must comply with independently approved, effective `.github/private-ci-approved-repositories.txt` **and** `.github/private-ci-carrier-contracts.json`, plus protected-main/V3 security review.

## Frozen review subject, 2026-11-13 UTC

- Source: `ores-truffle-oreslang/oreslang-source.llvm` (numeric GitHub repository ID `1410897874`).
- Candidate source PR: [#1728](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/1728).
- Source HEAD: `a4922dfd9680d5c5bb753bf8c01899f477f13f4e`.
- **Exact root Git tree SHA:** `2967d9aebd78a63e90258ffef30187b692fb879f`.
- Tree inventory: **1,839 blobs** (1,825 `100644`, 14 `100755`), total 9,397,433 file bytes; 92 nested directory tree entries. Mirror **every** blob, including all scripts, test fixtures, executable modes and nested paths. The 62-blob compiler subset is insufficient for full-tree acceptance.
- Already named PRIVATE carrier: `streempilot-test/browser-studio-e2e` (numeric repository ID `1323599136`; default `main` currently `0fd571f333009bb3365ae9932f2ee9fc464f5b45`).
- Candidate branch-only workflow: `.github/workflows/atto-llvm-1728-logical-punctuation-aot.yml` at Git blob `62c880ac27c5cbb9e93aa9fb44062b1187700db0`, carrier SHA `e3eca5a62d7c5925c687ade880b77f7de4b107b0`. **This is not a protected/pinned approved workflow identity**: a reviewer must independently approve a trusted workflow and immutable test-plan blob, not silently reuse a PR-controlled branch workflow.
- Source-main carrier contract currently lists this destination with a pinned workflow/test plan for **LLVM PR #787 only**, not #1728. **Independent approval and contract activation are outstanding.**

## Executed evidence already available, *not* full-tree certification

[Carrier run 43336001714](https://github.com/streempilot-test/browser-studio-e2e/actions/runs/43336001714), job `131592210471`, completed SUCCESS with actual runner steps: 62 hash-verified private source/test blobs, LLVM19/Clang19 native compiler build, O0/O2 native guest exit 42, RHS trap suppression/reached-RHS traps, XOR LTR exactly-once output, ten compiler negatives. Source head, tree and subset count were recorded. The carrier explicitly printed `FULL_TREE_MIRRORED=false`.

The source-org's current PR #1728 Actions jobs reported `steps=null`, a runner-admission failure, **not** evidence of semantic test failure. Neither observation meets the full-tree merge gate.

## Requested independent review and fail-closed acceptance

1. Independently approve PR #1728 + immutable source head/tree, destination ID and private/active status, least-privilege membership, checked source/destination organization boundary and restricted retention/cleanup. Source author cannot self-approve.
2. Independently review the destination **protected base** and workflow path/blob, permitted pipeline/test-plan blob, no untrusted-workflow code or privileged checkout, `contents: read` only, no PAT/Actions secrets/cross-org checkout token, no network exfiltration. Activate the exact, review-bound source-main carrier contract or a valid V3 independent approval receipt **before** any full-tree copying.
3. For each transfer, revalidate source/destination numeric IDs, visibility, current exact source commit/root Git tree and approved policy. Build source bytes from the first-party GitHub connector. A foreign Git blob SHA **cannot** simply be referenced in destination Git `create_tree`; GitHub rejects it with 422 until that blob has been materialized in the destination. Thus mirror each actual UTF-8 source file byte-for-byte, retain `100755` execute bits, and verify exact Git blob SHA after each destination write.
4. Reconstruct the private snapshot as its own **nested Git subtree**; assert its computed Git tree SHA **equals** `2967d9aebd78a63e90258ffef30187b692fb879f`. Do not confuse the enclosing carrier commit's root tree SHA with the nested source root. Reject missing, duplicated, unexpected files, symlink/submodule mutations (if any appear on fresh audit), changed modes, wrong hashes, and changed source HEAD. Fail before tests if exact full-tree identity is absent.
5. Run CMake/CTest on the copied full source tree, including `native_logical_punctuation_operators` and the selected real compiler-backed native parity suite. Build `oreslang-llvmc` from that tree. Test both O0 and O2 compiled guest binaries, exact exit 42, short-circuit and XOR exactly-once side effects, typed negative cases, and no emitted IR on compiler rejection.
6. Collect immutable evidence: source root tree hash, source PR head, destination nested tree SHA, destination immutable commit, approved workflow and plan blob IDs, source manifest SHA, runner ID/job attempt, `steps.length > 0`, native CTest + guest output, non-zero actual test assertions, stdout/exits, compile-negative diagnostics, source-free canary classification.
7. Require **offline negative-mutation tests proving zero private source writes** for stale source head/tree, wrong destination ID/name/privacy, altered workflow blob, revoked approval, wrong test plan, source SHA mismatch, malicious path/symlink/mode, incomplete transfer or insufficient budget. Never use a different organization simply because runner minutes are available.
8. After independent review and all checks pass, separately request code owner approval and merge the approved PR wave. Do not downgrade required checks or merge this review-only PR as if it authorized a transfer.

This document is an auditable approval request and changes **no active allowlist, carrier contract, or destination workflow**.
