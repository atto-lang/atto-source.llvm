# Review-only request: native LLVM #1056/#1082 exact-tree private carrier

This document prepares an **independent security review**. It is not authorization to copy or test Oreslang private source outside the source repository. This change must remain draft until all conditions below are demonstrably met.

## Scope

- Private source: `ores-truffle-oreslang/oreslang-source.llvm`, repository ID `1410897874`.
- Proposed **private** carrier: `usa-acc/usa-acc-e2e`, repository ID `1346346666`.
- Separate source candidates: LLVM #1056 (native borrow admission) and #1082 (reserved `unsafe` keyword). No other PR is authorized by this proposal.
- Restacked candidate heads (must be checked live again before an authorized run):
  - #1056: `3c09e3f3ea18631ed0c8ecb9db5714f2f12d488f`.
  - #1082: `fd8e35b459d006bebf32067d973291517f92f7bf`.
- Carrier review PR: `usa-acc/usa-acc-e2e#46`, current source-free reviewed-plan/workflow candidate `a59edbda3e648b02b5eaf302d244184c5ae28f2c` (the LLVM toolchain workflow now triggers for changes to either plan).
- Source-free reviewed plan Git blobs:
  - `ci/test-plans/llvm-pr1056.json`: `5530070140865f85983c494d20081879d66c850f`.
  - `ci/test-plans/llvm-pr1082.json`: `be0356199768f1e33cae777c68f1ae0457f874dd`.

## Proven source-free carrier capability (NOT guest LLVM source testing)

Carrier PR #46 source-free runs at exact reviewed head `d8705106829754389df5eab9784e50e93fc8289b`:
- LLVM toolchain, CMake/CTest and Clang ASan+UBSan: `40754207276`, success, five executed steps.
- Carrier verifier and Java toolchain: `40754207446`, success, eleven verifier steps and five Java-toolchain steps.
- Evidence contract: `40754207267`, success, eight executed steps.
- Source-bearing test jobs were **not** dispatched. Private source has **not** been copied into the carrier.


- New carrier head `a59edbda3e648b02b5eaf302d244184c5ae28f2c` has updated both source-free test plans and **executed** all three exact-head checks successfully: LLVM SDK/CMake/ASan run [40760802209](https://github.com/usa-acc/usa-acc-e2e/actions/runs/40760802209) (5 steps); carrier verifier + Java canary [40760802286](https://github.com/usa-acc/usa-acc-e2e/actions/runs/40760802286) (11 + 5 steps, source-bearing jobs skipped); evidence-contract [40760802221](https://github.com/usa-acc/usa-acc-e2e/actions/runs/40760802221) (8 steps). All five source blob pins were read back from the exact source PR heads and matched the plans.

## Current-main reconciliation and exact-head admission evidence

- Current LLVM main at reconciliation: `6ca6a6911a6eeedc51e6865f87e8b73a5d604a3e`.
- #1056 exact root Git tree: `6f5c61d306ecc58a2eeda98071e9a7250db5487b`. Three scoped files differ; zero commits behind the checked main.
- #1082 exact root Git tree: `73ecb9ba0274da2851f317b4e5daef78862afa3d`. Two scoped files differ; zero commits behind the checked main.
- All five files pinned by the two carrier plans were re-fetched from these heads; all Git blob SHAs exactly match the existing plan blobs. The two plans were re-pinned to the new CMake/Parser blobs; both use `--no-tests=error` in targeted and full-suite CTest commands, with source-free negative regression tests.
- On newly restacked source heads, native smoke [#1056 run 40760426440](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/actions/runs/40760426440), [#1082 run 40760599756](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/actions/runs/40760599756), and governance [run 40761123492](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/actions/runs/40761123492) each failed **before runner admission**, with `steps=null`; **NOT_EXECUTED**. At those earlier source heads neither feature received a native-source test verdict; newer #1056 evidence below must be evaluated separately.
- Earlier source CI attempts are also **NOT_EXECUTED**: #1056 [LLVM smoke 40749444863](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/actions/runs/40749444863), #1082 [LLVM smoke 40749583177](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/actions/runs/40749583177), governance [40739371331](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/actions/runs/40739371331). Sampled jobs all failed with `runner_id=null` and `steps=null`. These do not prove native compiler, CTest, or governance regression outcomes.

## Mandatory fail-closed gates before any export

1. Resolve platform-conformance P0 incidents #22, #77 and #127. Each must have immutable identity, `state=closed`, and `state_reason=completed`. As audited on 2026-10-28, **all three were open**.
2. Enable and independently verify **source main branch protection**, including required CODEOWNER approval, stale-review dismissal, at least one independent approval, administrator enforcement, no force push/delete, and no bypass. Source `main.protected=false` at audit time. Source `CODEOWNERS` alone does not enforce protection.
3. Obtain independent security review for this exact source governance head. Do not merge the proposed destination allowlist in an unprotected state.
4. Obtain independent carrier review on the exact #46 head; merge through an approved method to carrier main. Since carrier main was also unprotected, its exact approved-head/tree evidence must satisfy the audited **review-attestation fallback** if protection cannot be enabled. No author self-approval.
5. **Separately** update `.github/private-ci-carrier-contracts.json` under source protected-main review to pin carrier live repository ID, final reviewed main SHA, immutable workflow blob, final two test-plan blobs and `allowed_prs=[1056]` / `[1082]`. The current source main contract does **not** contain this carrier, so this allowlist-only proposal is deliberately **insufficient** to authorize an export.
6. Immediately before export, run the source publisher's entire trusted admission check against current source main: independent reviewer identity, protected governor bytes, incident closure, private destination identity, no symlinks/submodules, exact current PR heads and complete root Git tree identity, approved contract and carrier-main/workflow/plan bytes. No direct connector write may bypass those checks.
7. Only after steps 1–6, create one-use authorization and `ci/oreslang-<exact-source-root-tree>` temporary ref in the private carrier. Use its own scoped `GITHUB_TOKEN` without cross-org checkout tokens or secrets. Execute Release compiler build, targeted `native_compile_time_borrow_escape_admission` or `native_reserved_java_keywords` CTest and full native CTest; require real job/runner steps and bind evidence to exact SHA/tree and PR.
8. Verify cleanup authorization and delete source-bearing refs after testing. Follow up on source-object retention, logs and artifacts; branch deletion alone does not erase Git objects.

## Explicit exclusions

No transfer to public or internal repositories. No private LLVM blobs or source-bearing Git objects in this PR. No source export while source main remains unprotected, any P0 incident remains open, or review/contract is missing. Do not use older authority PR #728 (scoped to #725/#726/#729) or source PR #910 (a different proposed carrier) as authorization for these PRs.

## Required review outcome

Reviewers may revise or reject this proposed destination. Until every gate is verified, retain the PR as a draft and the current source allowlist on `main` as deny-by-default. Recheck all SHA bindings if either source PR, carrier plan, source governor or carrier main changes.

## October 28 independent verification update

- #1056 source head `3c09e3f3ea18631ed0c8ecb9db5714f2f12d488f`, root Git tree `6f5c61d306ecc58a2eeda98071e9a7250db5487b`, is now included in current LLVM main; the focused native LLVM19 carrier run [40766889048](https://github.com/honeypot-r-us-test/hnpt-lib-core/actions/runs/40766889048) has genuinely executed steps, but its **separate source-bearing transfer** is under independent [security review #212](https://github.com/ores-truffle-oreslang/oreslang-platform-conformance/issues/212). Successful native compiler tests do not retroactively authorize that export. No full-repository CI claim.
- #1082 latest source head `fd8e35b459d006bebf32067d973291517f92f7bf`, tree `73ecb9ba0274da2851f317b4e5daef78862afa3d`, is zero commits behind checked main; only Parser.cpp and the keyword harness differ. This source has **not** been run natively on an authorized alternate carrier.
- Carrier #46 current source-free review candidate `a59edbda3e648b02b5eaf302d244184c5ae28f2c` now pins Rust borrow harness `tests/native_borrow_admission.rs` (not the retired Python harness) for #1056, with new plan blob `5530070140865f85983c494d20081879d66c850f`. #1082 plan blob is `be0356199768f1e33cae777c68f1ae0457f874dd`. All 5 current Git blobs were fetched from exact source heads and matched. Targeted/full CTest commands require `--no-tests=error`.
- New carrier exact-head [LLVM toolchain 40774231226](https://github.com/usa-acc/usa-acc-e2e/actions/runs/40774231226) (5 steps) and [security 40774231211](https://github.com/usa-acc/usa-acc-e2e/actions/runs/40774231211) (11 verifier and five Java steps) are **SUCCESS**; [evidence 40774231472](https://github.com/usa-acc/usa-acc-e2e/actions/runs/40774231472) is pending as of this update. These are source-free jobs, not source compiler certification.
- Source `main.protected=false`; approved private destination list on `main` is empty; this draft does not add an executable carrier contract. P0 incidents #22/#77/#127 and newly opened independent review #212 remain open. **Private source export is on hold.**
