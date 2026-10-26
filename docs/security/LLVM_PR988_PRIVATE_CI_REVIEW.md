# LLVM #988 — proposed private CI carrier review (NOT an authorization)

**Disposition: REVIEW ONLY.** This file does not approve any private-source
transfer, add an allowlist entry, change protected repository policy, or
authorize automated publication. No LLVM private source has been sent to the
proposed carrier as part of this proposal.

## Candidate, fixed scope

- Source: `ores-truffle-oreslang/oreslang-source.llvm`, **PR #988 only**
- Exact source commit for first qualification:
  `7e7ca8e30f05eb2b75f1288eac209437f224c975`
- Exact source Git tree:
  `983d66d7b813b6a9abd40e0649ded65fe40d4cc9`
- Proposed destination: **private** `sonus-auris/sonus-auris.infra`
- Destination immutable GitHub repository ID: `1271706990`
- Source-free proof: Actions run
  [40225924937](https://github.com/sonus-auris/sonus-auris.infra/actions/runs/40225924937)
  executed real runner steps and passed.
- Fresh carrier-capacity confirmation on October 25: private-repo Actions runs
  40241181625 and 40241178103 both completed successfully, confirming that the
  proposed destination currently has executable GitHub Actions capacity.
- Source-free carrier review PR: `sonus-auris/sonus-auris.infra#29`, current head
  `410a250b6ca6b562d650e8195063fa47e6bf7a7a`, reviewed root tree
  `793d688f02f6a213ac5ccce2ba1a9b72a3ac34c0`, based on
  `980a9e8f2a5d2d87b0b49b8c4d764c1ccddd238e`.
- Reviewed workflow candidate blob:
  `8528a2cb43788fc86a4adbffead671c70a8b5710`.
- Reviewed PR #988 test-plan candidate blob:
  `7bcc58972d329d5f02bc853fa1d908d3d24cc570`.
- Current trusted-plan runner blob: `3175155f05565cf783533e10aecb08e12f80663d`.
- Exact-head source-free carrier run `40268961769` at `410a250b6ca6b562d650e8195063fa47e6bf7a7a` completed successfully across verifier, isolation, tokenless GraalVM/Maven, native CMake/CTest, and Rust offline probes.
- Receipt-bound synthetic source-bearing run `40269232700` completed successfully at synthetic carrier commit `fb463aa08c4435014f3402ebcebf2258538805e7`, proving exact authorization, tokenless pre-source toolchain setup, exact-tree provenance, gitless read-only sandbox preparation, and deprivileged network-isolated execution. The synthetic source tree was `fd0d58d34b5ce77095496b5a85fe252a1b72ec78`; one-use refs were revoked after evidence capture.
- The source-bearing workflow has no `contents: write` or `actions: write` authority, and all three pinned GraalVM setup steps explicitly receive an empty GitHub token with update checks disabled.
- Historical source-free baseline run `40253715185` completed successfully at older
  carrier head `c8bd315f6fd69c22dfd1e3f582ef8a0a53f9e9b3`: pinned checkout,
  eight carrier-verifier regressions (including exact CODEOWNERS and duplicate-key
  JSON rejection), six trusted-plan-runner regressions, and the exact LLVM PR #988
  plan lint all executed and passed; the source-bearing job was skipped as intended.
  Carrier CODEOWNERS blob: `0110a557c162204a399ebf1e628e50c9ed991439`.
  Verifier blob: `f235c9a1056da5c6b8149643010c82b6b2dc984a`.
  Historical trusted-plan runner blob: `53d0b80cabb6ca2f70df541132fd13c12ca932e9`.
  Trusted-plan runner-test blob: `d168051f3cbcbdc2ff1583bbe411fd61fd9d63ae`.
  Plan-lint blob: `50ff454ee43557a888fa5158beff808124157ba1`.
  The trusted-plan regression emitted `PLAN_SANDBOX_PASS uid=65534 pid=1 netns=isolated`, proving the reviewed runner entered fresh network + PID namespaces, dropped to UID/GID 65534, cleared effective capabilities, and enforced `NoNewPrivs` before executing the synthetic plan. A synthetic parent `GITHUB_TOKEN` was also removed before guest execution (`ENV_SCRUB_PASS`), and symlinked source entries are rejected. The source-bearing job now has a 180-minute ceiling consistent with the reviewed plan's bounded command timeouts, and it revalidates the exact dispatcher authorization ref both before and after source execution.
- Proposed protected source-policy delta, for independent review only:
  add exact allowlist entry `sonus-auris/sonus-auris.infra`; add a carrier
  contract with repository ID `1271706990`; after carrier PR #29 is independently
  approved and merged, pin the **resulting carrier `main` commit SHA** as `base_sha`
  and independently verify that its root tree equals the reviewed PR-head tree
  `793d688f02f6a213ac5ccce2ba1a9b72a3ac34c0`; do not use the PR-head commit itself
  as `base_sha` unless it is literally the resulting `main` commit. Pin workflow blob
  `8528a2cb43788fc86a4adbffead671c70a8b5710` and test-plan blob
  `7bcc58972d329d5f02bc853fa1d908d3d24cc570` restricted to PR `988`.
  This proposal is not effective authorization until independently reviewed
  and merged under source-repository policy.
- The previously successful common-source authority mirror is **not** proof
  that exporting LLVM source is authorized.

## Required independent security approval before any mirror

The current source `.github/private-ci-approved-repositories.txt` has **no
approved destinations**, and `.github/private-ci-carrier-contracts.json`
covers only PR #669/#674 in another carrier. Both are enforced review
boundaries. The requester of the tests must not self-approve a new carrier.

1. Obtain independent maintainer and `@ORESoftware` CODEOWNER/security
   approval against protected `main` or the source's enforced V3 exact
   review process, including candidate numeric repository ID.
2. Approve **one** immutable destination repository, one pinned workflow
   blob, one immutable test-plan blob, and allowed PR #988 only. No
   wildcards or opportunistic fallback repositories.
3. Independently review the destination workflow and runner permissions.
   Pin any third-party Actions by full SHA, use `contents: read`,
   `persist-credentials: false`, no source-org tokens or secrets,
   avoid untrusted fork events, and keep artifacts/logs private with bounded
   retention.
4. Verify that the exact source tree has no secrets or prohibited paths
   before transfer. Copy Git objects only after approval, retaining every
   blob SHA, tree path and mode; reject truncated or incomplete listings.
5. At the destination, require
   `git rev-parse HEAD:mirror/oreslang-source.llvm` to equal the **reviewed**
   source Git tree. Source identity must be checked before build/test.
6. Run the real native toolchain (document version, LLVM/CMake/Clang),
   configure/build `oreslang-llvmc`, run CTest
   `native_opaque_any_explicitly_unsupported` and
   `native_opaque_unknown_explicitly_unsupported`, plus
   `native_opaque_any_return_explicitly_unsupported` and
   `native_opaque_unknown_return_explicitly_unsupported`, then relevant native
   parser, ownership, and security regressions. Preserve complete logs,
   exact compiler revision, exit statuses and job/step receipts.
7. Fail closed on Actions zero-step failures, skipped checks, missing
   tools, parser failures unrelated to the expected diagnostic, stale
   source, or any discrepancy in Git tree SHA.

## Freshness and reconciliation

This proposal tracks LLVM #988's current-main two-parent reconciliation at
`7e7ca8e30f05eb2b75f1288eac209437f224c975`. The resulting PR diff contains `Parser.cpp`, the CTest registrations in `CMakeLists.txt`,
`tests/contract.py` and `tests/test_contract_harness.py` fail-closed
infrastructure diagnostic checks, two byte-identical canonical parameter
fixtures, and two new opaque return-position fixtures.
A subsequent #988 hardening round adds `--parser-diagnostic` to the four
opaque-type admission CTests, requiring the native compiler's exact single-line
`oreslang-llvmc: <expected> at byte N` error format. Wrapper-prefixed output,
multiple stderr lines and malformed offsets are rejected by new fake-compiler
regressions under Python `-O` and `-OO`. These tests have **not** been executed
with a native LLVM compiler or via hosted CI, and changing the pinned source
head/tree requires renewed independent security review. A standalone local
matching-predicate check is not a native or committed-harness test pass.

A follow-up hardening round now binds all four opaque-type CTests to the
reviewed fixture Git blob SHA with `--fixture-git-blob`, rejects malformed
blob identifiers, and redacts raw operating-system exceptions so missing
compiler/fixture paths cannot leak into CI logs. Focused local adversarial
execution of these new predicates passed 14/14 cases across Python `-O` and
`-OO` (exact identity, mutation rejection, strict blob syntax, compiler and
fixture path redaction, wrapper-prefix spoof rejection, and infrastructure
spoof rejection). This focused proof is not the complete committed Python
harness and not a native compiler/CTest pass. Source-hosted exact-head runs at
`7e7ca8e30f05eb2b75f1288eac209437f224c975` again completed with zero
executed steps; treat them as infrastructure-only failures. In particular,
workflow runs 40250242841 (LLVM conformance contract) and 40250242960
(LLVM compiler and native execution) each produced a failed job with an empty
step list.

The pinned head is a two-parent current-main reconciliation: first parent
`104863ddfc166854b9e44986a1a71d5fb4ed60f6`, second parent
`a80fe00c5a68b8232e21cad0b58fd4858fa9ec53`. The reconciled tree preserves
current `main` and overlays only the reviewed eight-file PR surface; GitHub
compare reports 16 ahead / 0 behind.

Native LLVM include/link and wake-test repairs were already present on newer
`main` and were deliberately not rolled back. Any subsequent PR-head/tree
change invalidates this proposal's exact source identity and requires renewed
security review before transfer.

## Claims expressly out of scope

The two #988 fixtures test deliberate rejection of unsupported native
`any`/`unknown`. Passing those CTests does **not** establish tagged
dynamic-value lowering, actor transport, capability permission enforcement,
Java/LLVM semantic parity, or executable guest runtime support.

**Next action:** security owners decide whether to authorize this exact
carrier and plan. Do not publish source until their reviewed decision is
effective.

## Synthetic source-bearing carrier proof

A harmless synthetic source tree was used to exercise the full carrier path without exporting LLVM source:

- source repo label: `synthetic/private`, PR `7`
- synthetic source tree: `a379330e6f4461f78786ec83d7ec34431aeca043`
- receipt-bound carrier commit: `0497162489b548c8bbfda21701935e40229ac79a`
- synthetic self-test plan blob: `46614ef8e2c92055b4e7c9f06cc88d9a64b936fa`
- Actions run `40265226955`: success
- live carrier identity, initial authorization ref, exact-tree provenance, pre-test authorization recheck, sandbox execution, and final authorization recheck all passed
- sandbox evidence: `PLAN_SANDBOX_PASS uid=65534 pid=1 netns=isolated`
- guest evidence: `SYNTHETIC_SOURCE_BEARING_PASS`

This is carrier-mechanics evidence only. It is not LLVM #988 source execution and does not authorize private-source transfer.

### Authorization negative controls

- Run `40265223443`: the `ci/oreslang-*` branch existed at its reviewed parent but no exact `ci/authorized-<GITHUB_SHA>` ref existed. Carrier identity passed, authorization failed in step 2, and checkout/provenance/source execution were skipped.
- Run `40265492280`: exact authorization passed initial and pre-test checks, the sandboxed synthetic command completed successfully, then the authorization ref was deliberately moved while execution was in flight. The final recheck failed with `dispatcher authorization revoked or changed during tests`, so the overall workflow failed.

These controls prove branch naming is insufficient authority and mid-run revocation prevents a green carrier result.

## Current hardened synthetic source-bearing proof

The final current candidate was exercised with harmless synthetic source after removing third-party carrier-token access and all source-bearing workflow write authority:

- carrier PR #29 head: `410a250b6ca6b562d650e8195063fa47e6bf7a7a`
- carrier root tree: `793d688f02f6a213ac5ccce2ba1a9b72a3ac34c0`
- workflow blob: `8528a2cb43788fc86a4adbffead671c70a8b5710`
- runner blob: `3175155f05565cf783533e10aecb08e12f80663d`
- LLVM #988 production plan blob: `7bcc58972d329d5f02bc853fa1d908d3d24cc570`
- exact-head source-free run: `40268961769` — success
- synthetic source-bearing run: `40269232700` — success
- synthetic carrier commit: `fb463aa08c4435014f3402ebcebf2258538805e7`
- synthetic source tree: `fd0d58d34b5ce77095496b5a85fe252a1b72ec78`
- evidence markers: `SOURCE_DISPATCH_AUTH_PASS`, `CARRIER_PROVENANCE=PASS`, `CARRIER_AUTHORIZATION_RECHECK_PASS`, `GITLESS_SANDBOX_PREP_PASS`, `SYNTHETIC_SOURCE_BEARING_PASS`, and `TEST_PLAN_PASS commands=1 source=synthetic/private pr=7`.

The first run against the redesigned PID namespace failed only because the old synthetic command asserted that it, rather than the trusted plan runner, was PID 1. The test was corrected to require the command's parent PID to be 1; production LLVM #988 plan bytes were not changed. The corrected run passed end to end.

This remains carrier-mechanics evidence only. No LLVM #988 private source has been transferred or executed.
