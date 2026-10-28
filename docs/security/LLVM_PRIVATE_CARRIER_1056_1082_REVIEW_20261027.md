# Review-only request: native LLVM #1056/#1082 exact-tree private carrier

This document prepares an **independent security review**. It is not authorization to copy or test Oreslang private source outside the source repository. This change must remain draft until all conditions below are demonstrably met.

## Scope

- Private source: `ores-truffle-oreslang/oreslang-source.llvm`, repository ID `1410897874`.
- Proposed **private** carrier: `usa-acc/usa-acc-e2e`, repository ID `1346346666`.
- Separate source candidates: LLVM #1056 (native borrow admission) and #1082 (reserved `unsafe` keyword). No other PR is authorized by this proposal.
- Restacked candidate heads (must be checked live again before an authorized run):
  - #1056: `1589c62b0cc802230ab58da11dd4352b4f78950d`.
  - #1082: `85cd7a5d1b8530617044f1b34a6322e22e4c4ec1`.
- Carrier review PR: `usa-acc/usa-acc-e2e#46`, current source-free reviewed-plan/workflow candidate `d8705106829754389df5eab9784e50e93fc8289b` (the LLVM toolchain workflow now triggers for changes to either plan).
- Source-free reviewed plan Git blobs:
  - `ci/test-plans/llvm-pr1056.json`: `2692e09c827641d1591b4d9993914c16e589dc28`.
  - `ci/test-plans/llvm-pr1082.json`: `4ae4a884a93c6b5ae3b00a69aa935a3a9a21fc14`.

## Proven source-free carrier capability (NOT guest LLVM source testing)

Carrier PR #46 source-free runs at exact reviewed head `d8705106829754389df5eab9784e50e93fc8289b`:
- LLVM toolchain, CMake/CTest and Clang ASan+UBSan: `40754207276`, success, five executed steps.
- Carrier verifier and Java toolchain: `40754207446`, success, eleven verifier steps and five Java-toolchain steps.
- Evidence contract: `40754207267`, success, eight executed steps.
- Source-bearing test jobs were **not** dispatched. Private source has **not** been copied into the carrier.

## Current-main reconciliation and exact-head admission evidence

- Current LLVM main at reconciliation: `06d73764d01a48b25857980a878088612b19e1c4`.
- #1056 exact root Git tree: `6e05158c7648c453426bf34d646dbb2d39c73da5`. Three scoped files differ; zero commits behind the checked main.
- #1082 exact root Git tree: `d7d15a738c905dd8caa2469dd3d0b7250a7bbb13`. Two scoped files differ; zero commits behind the checked main.
- All five files pinned by the two carrier plans were re-fetched from these heads; all Git blob SHAs exactly match the existing plan blobs. The two plans were re-pinned to the new CMake/Parser blobs; both use `--no-tests=error` in targeted and full-suite CTest commands, with source-free negative regression tests.
- Fresh source CI attempts are **NOT_EXECUTED**: #1056 [LLVM smoke 40749444863](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/actions/runs/40749444863), #1082 [LLVM smoke 40749583177](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/actions/runs/40749583177), governance [40739371331](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/actions/runs/40739371331). Sampled jobs all failed with `runner_id=null` and `steps=null`. These do not prove native compiler, CTest, or governance regression outcomes.

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
