# LLVM #988 — proposed private CI carrier review (NOT an authorization)


> **LATEST REFRESH — 2026-10-26 after LLVM #940**
>
> This section supersedes earlier claims that #988 is reconciled to the current
> source `main`.
>
> - current LLVM `main`: `aabe47f0eec3434c8456cee85766cad038ecd518`;
> - exact #988 head remains `2303994172d5918a5bcf96711ca92a2289e799f2`;
> - exact #988 tree remains `a04b40a3ac493c1791be6576ac69347b0f36ca82`;
> - current compare: **18 ahead / 2 behind**.
>
> Therefore #988 requires another current-main reconciliation before any
> source-bearing qualification. Earlier source-free and synthetic carrier
> receipts remain historical control/capability evidence only. This document is
> **NON-AUTHORIZING / REVIEW ONLY**: it changes neither the destination
> allowlist nor the carrier contract and grants no private-source export authority.
>


> **CURRENT REFRESH — 2026-10-26**
>
> This section supersedes older exact-source identity lines below for any future
> qualification. LLVM #988 is now reconciled to current source `main`.
>
> - current source `main`: `656ccceb564c778b41077f88c7257a0758043a33`
> - exact #988 head: `2303994172d5918a5bcf96711ca92a2289e799f2`
> - exact #988 tree: `a04b40a3ac493c1791be6576ac69347b0f36ca82`
> - parents: previous #988 head `c99e634f2601af90c2f477ec987babe2ae07f472`
>   and current `main` `656ccceb564c778b41077f88c7257a0758043a33`
> - GitHub compare: **18 ahead / 0 behind**
> - the seven non-CMake reviewed #988 blobs are unchanged; `CMakeLists.txt`
>   is a three-way reconciliation that preserves current-main native
>   String/CString host-test changes near line ~3896 and the reviewed four
>   opaque-type CTests near line ~757.
> - fresh source-hosted runs `40403944009` (conformance; job
>   `121856737457`) and `40403944639` (compiler/native; job
>   `121856740429`) both failed with `steps=null`; they are
>   runner-admission/infrastructure evidence only.
>
> Source-side governance hardening #1004 now has stepful alternate-org evidence
> for its current control bytes. The exact governance bundle tree
> `6bf04e3034758f24ebcc698a8639f7bb20e7e576` was executed in private
> `canonical-cloud-test/legacy-mirror-guard-e2e`; run `40402911627`, job
> `121853155001`, passed 6/6 audit tests, 42/42 publisher tests on each of
> three consecutive passes, shell syntax/bytecode checks, and the fail-closed
> structure audit. That run followed a stepful failure that exposed and fixed
> an exact-key bug in review-bypass validation. Current #1004 head
> `681261831d032cd3b4ef5b005b5d81a28e8b4326` preserves all ten tested
> governance/control blob SHAs.
>
> This refresh remains review-only. Source `main` still reports
> `protected=false`; the allowlist remains empty and the live carrier contract
> does not authorize PR #988. No LLVM private source transfer is authorized.


**Disposition: REVIEW ONLY.** This file does not approve any private-source
transfer, add an allowlist entry, change protected repository policy, or
authorize automated publication. No LLVM private source has been sent to the
proposed carrier as part of this proposal.

## Candidate, fixed scope

- Source: `ores-truffle-oreslang/oreslang-source.llvm`, **PR #988 only**
- Exact source commit for qualification:
  `c99e634f2601af90c2f477ec987babe2ae07f472`
- Exact source Git tree:
  `7d281a4645b257708e1b6e046d1ae9e17a5b3ef4`
- Proposed destination: **private** `sonus-auris/sonus-auris.infra`
- Destination immutable GitHub repository ID: `1271706990`
- Source-free proof: Actions run
  [40225924937](https://github.com/sonus-auris/sonus-auris.infra/actions/runs/40225924937)
  executed real runner steps and passed.
- Fresh carrier-capacity confirmation on October 25: private-repo Actions runs
  40241181625 and 40241178103 both completed successfully, confirming that the
  proposed destination currently has executable GitHub Actions capacity.
- Source-free carrier review PR: `sonus-auris/sonus-auris.infra#29`, current head
  `3f2fc48b652582fffae29bfc2da3e507690f6ebc`, reviewed root tree
  `bb514698ef757e2c7184cb79c2b80b90591ddb5c`, based on
  `980a9e8f2a5d2d87b0b49b8c4d764c1ccddd238e`.
- Reviewed workflow candidate blob:
  `c0678f9a6fc174f3dd50b4c9dd0b60615c0b207f`.
- Reviewed PR #988 test-plan candidate blob:
  `7bcc58972d329d5f02bc853fa1d908d3d24cc570`.
- Current trusted-plan runner blob: `3175155f05565cf783533e10aecb08e12f80663d`.
- Current dependency-cache verifier/test blobs:
  `cf3013e95c615dedc653c59852c21df9174de2fa` /
  `08a162f7790dd5fb19d4ade4815e69ff0e6ce3d6`.
- Current carrier-org run `40367099135` failed with zero executed steps and is infrastructure-only evidence.
- The exact current carrier tree was mirrored by Git objects into private `usa-acc/usa-acc-infra`; all 16 blob SHAs and the recreated root tree matched exactly. Alternate-org PR run `40369486755` completed successfully across verifier, dependency-cache manifest regressions, isolation, tokenless GraalVM/Maven, native CMake/CTest, and Rust offline probes.
- Historical full source-bearing run `40352545148` completed successfully at predecessor carrier head `d7eb5fb546c5d093af34a892721e998fe189d63e` and synthetic carrier commit `66194bacb70c5ca230c1a54cb07d7b8bc39b8fb1`. It predates dependency-cache sealing and is not current-head execution evidence.
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
  `bb514698ef757e2c7184cb79c2b80b90591ddb5c`; do not use the PR-head commit itself
  as `base_sha` unless it is literally the resulting `main` commit. Pin workflow blob
  `c0678f9a6fc174f3dd50b4c9dd0b60615c0b207f` and test-plan blob
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
`c99e634f2601af90c2f477ec987babe2ae07f472`. The resulting PR diff contains `Parser.cpp`, the CTest registrations in `CMakeLists.txt`,
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
`c99e634f2601af90c2f477ec987babe2ae07f472` again completed with zero
executed steps; treat them as infrastructure-only failures. In particular,
workflow runs `40372269598` (LLVM conformance contract; job `121745833577`) and
`40372269559` (LLVM compiler and native execution; job `121745834951`) each
produced a failed job with `steps=null`.

The pinned head is a two-parent current-main reconciliation: first parent
`7e7ca8e30f05eb2b75f1288eac209437f224c975`, second parent
`7b705bf7e8c8e24f119f95a910144388e44516b6`. The three intervening `main`
commits touched only `.github/CODEOWNERS` and `.github/workflows/ci.yml`, which
are disjoint from #988's eight-file surface. The reconciliation overlays the
same eight reviewed blob SHAs onto the new `main` tree; GitHub compare reports
17 ahead / 0 behind.

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

- carrier PR #29 head: `d7eb5fb546c5d093af34a892721e998fe189d63e`
- carrier root tree: `fbec48d1d61cb32ec87c8a8b0b714115fecfda93`
- workflow blob: `6a13b894ff9bab1aaddc0600e926d00d7e01a829`
- runner blob: `3175155f05565cf783533e10aecb08e12f80663d`
- LLVM #988 production plan blob: `7bcc58972d329d5f02bc853fa1d908d3d24cc570`
- exact-head source-free run: `40351259539` — success
- synthetic source-bearing run: `40352545148` — success
- synthetic carrier commit: `66194bacb70c5ca230c1a54cb07d7b8bc39b8fb1`
- synthetic source tree: `f700fabc2a274281fb521d2a899eaafbb50b4ea5`
- evidence markers: `SOURCE_DISPATCH_AUTH_PASS`, `CARRIER_PROVENANCE=PASS`, `CARRIER_AUTHORIZATION_RECHECK_PASS`, `GITLESS_SANDBOX_PREP_PASS`, `SYNTHETIC_SOURCE_BEARING_PASS`, and `TEST_PLAN_PASS commands=1 source=synthetic/private pr=7`.

The first run against the redesigned PID namespace failed only because the old synthetic command asserted that it, rather than the trusted plan runner, was PID 1. The test was corrected to require the command's parent PID to be 1; production LLVM #988 plan bytes were not changed. The corrected run passed end to end.

This remains carrier-mechanics evidence only. No LLVM #988 private source has been transferred or executed.


### Current post-guest revocation proof

The current workflow now revalidates the exact dispatcher authorization ref after guest execution, with strict duplicate-key-rejecting JSON parsing and exact ref/type/SHA binding at every lifecycle check.

- exact carrier head: `d7eb5fb546c5d093af34a892721e998fe189d63e`
- exact carrier root tree: `fbec48d1d61cb32ec87c8a8b0b714115fecfda93`
- workflow blob: `6a13b894ff9bab1aaddc0600e926d00d7e01a829`
- exact-head source-free run: `40351259539` — success
- current-workflow positive source-bearing run: `40352545148` attempt 2 — success, including `CARRIER_AUTHORIZATION_FINAL_PASS`
- long-window revocation run: `40270407891` — expected failure
- revocation carrier commit: `7ab7df8c5f32dddecb52272805e5895bd28de73a`

For run `40270407891`, initial authorization, exact-tree provenance, pre-guest recheck, gitless sandbox preparation, and the guest command all succeeded. The authorization ref was revoked after pre-guest admission while the 90-second guest command was running. The guest emitted `SYNTHETIC_LONG_REVOCATION_EXEC_PASS` and `TEST_PLAN_PASS`; the post-guest step then rejected the run with `dispatcher authorization revoked or changed during guest execution`. This proves a revoked authorization cannot produce a green qualification even after source execution itself passes.

The temporary synthetic refs were revoked after evidence capture. No LLVM private source was transferred or executed.


## Final early prefetch admission proof

The final carrier candidate now verifies source-bearing receipt/control provenance **before any GraalVM/networked dependency setup or execution of the prefetch controller**.

Current immutable pins:
- carrier PR #29 head: `d7eb5fb546c5d093af34a892721e998fe189d63e`
- carrier root tree: `fbec48d1d61cb32ec87c8a8b0b714115fecfda93`
- workflow blob: `6a13b894ff9bab1aaddc0600e926d00d7e01a829`
- trusted-plan runner blob: `3175155f05565cf783533e10aecb08e12f80663d`
- LLVM #988 production plan blob: `7bcc58972d329d5f02bc853fa1d908d3d24cc570`

Before dependency setup, the carrier requires the exact authorization ref, exact one-parent destination base, live bounded receipt expiry, destination repo/id binding, exact source-tree/ref binding, source repo/id/head/PR identity, strict reviewed-plan source/PR/digest binding, exact workflow/controller/plan Git blobs from the reviewed parent, canonical commit-message receipt digest, and a diff restricted to the receipt plus `private-oreslang-source/**`.

Executed evidence:
- exact-head source-free run `40351259539`: success
- clean receipt-bound synthetic source-bearing run `40352545148`: success, including `PREFETCH_CONTROL_PROVENANCE_PASS`, `CARRIER_PROVENANCE=PASS`, `SYNTHETIC_SOURCE_BEARING_PASS`, `TEST_PLAN_PASS`, and `CARRIER_AUTHORIZATION_FINAL_PASS`
- exact-authorized plan/PR mismatch run `40351926177`: expected failure before GraalVM with `prefetch source PR not authorized by plan`
- exact-authorized mutated-prefetch-controller run `40353765301`: expected failure at immutable prefetch-control provenance; GraalVM and controller execution were skipped

The temporary synthetic refs were revoked after evidence capture. These runs prove only the carrier mechanics. No LLVM #988 private source was transferred or executed.


## Current dependency-cache artifact boundary proof

The current carrier head adds a cross-job dependency-cache manifest boundary. The producer seals every dependency-cache regular file into a canonical manifest and passes only the manifest SHA-256 through the trusted job output. The consumer revalidates the downloaded manifest and every file byte after exact carrier provenance and before guest preparation.

Current pins:
- carrier head: `3f2fc48b652582fffae29bfc2da3e507690f6ebc`
- carrier tree: `bb514698ef757e2c7184cb79c2b80b90591ddb5c`
- workflow blob: `c0678f9a6fc174f3dd50b4c9dd0b60615c0b207f`
- dependency-cache verifier: `cf3013e95c615dedc653c59852c21df9174de2fa`
- dependency-cache tests: `08a162f7790dd5fb19d4ade4815e69ff0e6ce3d6`
- LLVM #988 plan: `7bcc58972d329d5f02bc853fa1d908d3d24cc570`

The verifier rejects symlinks, hardlinks, special files, unsafe paths, duplicate/noncanonical JSON, count/size overflow, unexpected files, and byte mutations.

Executed evidence:
- `usa-acc/usa-acc-infra` capacity canary `40368244501`, job `121731684187`: success with real steps.
- Exact Git-object mirror commit `6d5ac86c81ba31d824be16f21c5a127f1f93766f` has root tree exactly `bb514698ef757e2c7184cb79c2b80b90591ddb5c`.
- Exact-tree source-free run `40369486755`: success.
- Synthetic artifact integration run `40369770317`: success. Good round-trip emitted `DEPENDENCY_CACHE_VERIFY_PASS files=2 bytes=11` and `CACHE_ARTIFACT_ROUNDTRIP_PASS`; a cache mutated after sealing was rejected with `DEPENDENCY_CACHE_DENIED: dependency-cache bytes differ from sealed manifest` and `CACHE_ARTIFACT_TAMPER_DENIED_PASS`.

This alternate-org evidence contains no LLVM private source. The production source-bearing workflow at the current head still lacks a stepful run because the current carrier org stopped admitting runners; predecessor source-bearing evidence must remain classified as historical.


## Independent alternate-org corroboration

Because the current carrier org stopped admitting runners, the exact current carrier tree
`bb514698ef757e2c7184cb79c2b80b90591ddb5c` was also recreated as a content-addressed
subtree in private `canonical-cloud-test/legacy-mirror-guard-e2e`. Every mirrored blob SHA
matched the source and the recreated subtree SHA matched exactly.

Fallback run `40370992905`, job `121741366347`, completed successfully and emitted
`EXACT_CARRIER_TREE_PASS bb514698ef757e2c7184cb79c2b80b90591ddb5c`.
The exact subtree then passed the carrier regression suites, reviewed-plan validation,
dependency-cache seal/verify plus deliberate tamper rejection, and a deprivileged isolated
guest-boundary probe. The temporary fallback branch was moved back to its original main
commit after evidence capture. This proof contains no LLVM private source.

Review-branch reconciliation commit: `19d3cdc3dec5378f32dbf06376fa94bd04bd275f`, bringing this review branch onto source `main` `7b705bf7e8c8e24f119f95a910144388e44516b6` without changing the reviewed #988 source blobs.
