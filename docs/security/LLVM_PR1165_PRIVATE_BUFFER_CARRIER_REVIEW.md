# LLVM PR #1165 private buffer CI carrier — review proposal (HOLD)

**Disposition: NON-AUTHORIZING. No private LLVM source transfer permitted.**
This file documents a proposed private carrier and evidence; it does **not**
change the protected authoritative destination allowlist or carrier contracts,
authorize a GitHub token / credential, or approve copying Git objects. Source
PR #1165 and source PR #1161 remain draft and unmerged.

## Exact source to be qualified

- Private source: `ores-truffle-oreslang/oreslang-source.llvm`, ID `1410897874`
- Source PR: [#1165](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/1165)
- Observed immutable head: `ff0e9055309c426fc674a92feab4be26b4819c82`
- Full source root Git tree SHA: `29e0729d0ce16cd91c4ed5df477ccb669e1c02ee`
- Base: `feat/trusted-actor-buffer-move-20261029` (#1161).
- Observed source `main`: `6ca6a6911a6eeedc51e6865f87e8b73a5d604a3e`
- Source `main` `protected=false` (mandatory deny).

This exact source SHA is a **frozen candidate only**. Any update of #1165
requires a fresh source HEAD, complete root tree and security review. Do not
use a moving PR ref as source transfer authority.

## Verified source-free runner candidate

- Destination: private `honeypot-r-us-test/hnpt-e2e`
- Immutable destination repository ID: `1346492776`
- Observed destination `main` SHA: `537c5f1c4b0249ec5e4f016d82891cd74507bf86`
- Destination `main` `protected=false` (requires reviewed V3 attestation).
- Source-free candidate PR [#19](https://github.com/honeypot-r-us-test/hnpt-e2e/pull/19),
  head `872423dd9c3e32f62d632b26cb1dc1eafccf0678`,
  **draft, not merged**.
- Source-free canary workflow
  `.github/workflows/source-free-llvm-buffer-canary.yml`
  blob `ed2d9be81726c7d9a1953da2ee57e98b8dab5b35`.
- Exact-head canary [run 40953347121](https://github.com/honeypot-r-us-test/hnpt-e2e/actions/runs/40953347121):
  completed `success`, worker job `Native GCC and Clang source-free execution`
  completed `success`. Executed steps: `Set up job`,
  `Execute native C++17 without checkout or private source`,
  `Complete job`, all successful. This proves that an actual runner
  compiled and executed a self-contained C++17 ASan/UBSan sample using GCC
  and Clang. No secret, checkout, private Oreslang source, external network
  dependency or publication was used.
- Separate recent carrier execution evidence:
  [40950785361](https://github.com/honeypot-r-us-test/hnpt-e2e/actions/runs/40950785361)
  completed actual `exact-tree-host-tests` steps, but is **not**
  qualification evidence for source PR #1165.

### Authorization blockers (independent)

1. **Protected source:** authenticated branch read found
   `ores-truffle-oreslang/oreslang-source.llvm/main protected=false`.
   `scripts/publish-private-ci-source-v3.py` explicitly refuses
   source exports without enforced protected-source branch review.
2. **Incident gates:** the exact platform-conformance issues
   [#22](https://github.com/ores-truffle-oreslang/oreslang-platform-conformance/issues/22),
   [#77](https://github.com/ores-truffle-oreslang/oreslang-platform-conformance/issues/77)
   and [#127](https://github.com/ores-truffle-oreslang/oreslang-platform-conformance/issues/127)
   are **open** on this audit. V3 requires each to be closed with exact
   `state_reason=completed` before it may read/export source.
3. **Protected policy:** `.github/private-ci-approved-repositories.txt` has
   no uncommented destinations. `.github/private-ci-carrier-contracts.json`
   contains only an older plan for another destination and PRs #669/#674.
   Neither permits PR #1165.
4. **Carrier code:** candidate PR #19 contains **only a source-free canary**.
   It is **not** a reviewed V3 carrier. A dedicated source-free,
   immutable, trusted-test-plan runner, Git tree verifier, workflow isolation
   proof and cleanup lifecycle must be independently reviewed first.
   A canary workflow blob is not a valid pinned V3 source-bearing workflow.
5. **Carrier governance:** destination `main` currently shows
   `protected=false`. V3's independently reviewed carrier-attestation
   requirements must be fulfilled or carrier main protection must be
   independently verified. A draft head is not the immutable approved
   `base_sha`.
6. **Executable source evidence:** LLVM source Actions for PR #1165
   report failures with zero executed steps. Host-only prior tests and
   source-free canary do not prove compiled `.ores` zero-copy messaging,
   full C++ sanitizer results on this head, or five-backend parity.

## Required independent follow-up (no automatic source export)

1. Have owners address incidents #22/#77/#127 with independently verified
   closure and `state_reason=completed`; never close solely to unlock CI.
2. Enforce actual review/required-status branch protection on source `main`;
   verify the live protection and CODEOWNERS policy independently.
3. Develop a source-free private V3 carrier PR based on the reviewed template,
   with a static `ci/test-plans/llvm-pr1165.json` restricted to PR #1165:
   run the exact-head four-test GCC/Clang ASan+UBSan and GCC TSan matrix,
   the compiler's `.ores` negative fixtures, and then the positive source
   fixture *only after the compiler/runtime implements it*. No arbitrary
   shell commands, `pull_request_target` guest execution, secrets or
   network dependencies.
4. Review and merge the carrier under valid governance; record its resulting
   **carrier main commit**, root tree, workflow blob and test-plan blob SHA.
   Verify personnel access, exact repository ID, retention and isolation.
5. In separately reviewed protected-source changes, add the **exact**
   `honeypot-r-us-test/hnpt-e2e` allowlist entry and the V3 carrier contract
   bound to ID `1346492776`, approved base SHA, workflow and test plan.
6. Only after all V3 guards pass, mirror the **entire immutable** source tree
   to a temporary private `ci/oreslang-<sha>` branch, verify per-blob SHAs
   and root-tree equality, run Actions locally in that carrier without
   cross-org checkout tokens, collect authenticated job-step and binary
   receipts, and remove/retire temporary refs under retention policy.

No steps above authorize source export through this proposal. Do not
reinterpret source-free execution as approval or skip branch protection.

## Follow-up source-free GCC/Clang/TSan confirmation

- Candidate PR #19 advanced to exact head
  `88515672877058769824e33729d397c3a8e0d7d4`.
- Latest source-free canary workflow blob:
  `3ab0cdc686067ef51785fc0bb2460d9b02e87586`.
- [Run 40953843035](https://github.com/honeypot-r-us-test/hnpt-e2e/actions/runs/40953843035)
  executed two jobs and both concluded **success**.
- Native GCC/Clang job `123728689996` actually emitted
  `RUNNER_CANARY_EXECUTED=g++ RUN_ID=40953843035` and
  `RUNNER_CANARY_EXECUTED=clang++ RUN_ID=40953843035` after separate
  compiler-generated ASan+UBSan binaries were executed.
- Native GCC ThreadSanitizer job `123728690226` emitted
  `RUNNER_TSAN_EXECUTED=yes RUN_ID=40953843035` after compiling and
  executing a source-free threaded C++17 test.
- No private LLVM source was present in either workflow. No permission
  is gained to transfer it; all source-protection, incident-closure,
  approved-carrier, attested-workflow, and test-plan blockers above remain.

This later result supersedes only the **runner capability** evidence; it
does not turn the source-free canary workflow into a V3 trusted publisher.
