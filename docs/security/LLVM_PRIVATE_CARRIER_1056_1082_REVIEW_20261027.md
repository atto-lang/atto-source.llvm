# Review-only request: native LLVM #1056/#1082 exact-tree private carrier

This document prepares an **independent security review**. It is not authorization to copy or test Oreslang private source outside the source repository. This change must remain draft until all conditions below are demonstrably met.

## Scope

- Private source: `ores-truffle-oreslang/oreslang-source.llvm`, repository ID `1410897874`.
- Proposed **private** carrier: `usa-acc/usa-acc-e2e`, repository ID `1346346666`.
- Separate source candidates: LLVM #1056 (native borrow admission) and #1082 (reserved `unsafe` keyword). No other PR is authorized by this proposal.
- Restacked candidate heads (must be checked live again before an authorized run):
  - #1056: `777ed287961f50503977c7e47a961eea3c4f8244`.
  - #1082: `73a9249ffb87f75eba1a3d50ea8f07ee15a4277d`.
- Carrier review PR: `usa-acc/usa-acc-e2e#46`, current source-free reviewed-plan/workflow candidate `68b3690a307ac017bfbb850bf8af02a4844ca271` (the LLVM toolchain workflow now triggers for changes to either plan).
- Source-free reviewed plan Git blobs:
  - `ci/test-plans/llvm-pr1056.json`: `5dfd1b740f7ea5989a34d2d5427fb7b117015d38`.
  - `ci/test-plans/llvm-pr1082.json`: `98b5de36de0c761d69d8563b61605345a56257f1`.

## Proven source-free carrier capability (NOT guest LLVM source testing)

Carrier PR #46 source-free runs at exact reviewed head `68b3690a307ac017bfbb850bf8af02a4844ca271`:
- LLVM toolchain, CMake/CTest and Clang ASan+UBSan: `40610637614`, success, five executed steps.
- Carrier verifier and Java toolchain: `40610637676`, success, eleven verifier steps and five Java-toolchain steps.
- Evidence contract: `40610637702`, success, eight executed steps.
- Source-bearing test jobs were **not** dispatched. Private source has **not** been copied into the carrier.

## Current-main reconciliation and exact-head admission evidence

- Current LLVM main at reconciliation: `d0c482d536656a6d768e165efed91f239994b76d`.
- #1056 exact root Git tree: `086d701ca920e9d5a2513fb00c8cb54e68465d4e`. Three scoped files differ; zero commits behind the checked main.
- #1082 exact root Git tree: `7c15b06822437ccf08017921a31eb13c0a4685d5`. Two scoped files differ; zero commits behind the checked main.
- All five files pinned by the two carrier plans were re-fetched from these heads; all Git blob SHAs exactly match the existing plan blobs. No plan mutation is required.
- Fresh source CI attempts are **NOT_EXECUTED**: #1056 [LLVM smoke 40739101327](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/actions/runs/40739101327), #1082 [LLVM smoke 40739255490](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/actions/runs/40739255490), governance [40739371331](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/actions/runs/40739371331). Sampled jobs all failed with `runner_id=null` and `steps=null`. These do not prove native compiler, CTest, or governance regression outcomes.

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
