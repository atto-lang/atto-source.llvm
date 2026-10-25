# LLVM #988 — proposed private CI carrier review (NOT an authorization)

**Disposition: REVIEW ONLY.** This file does not approve any private-source
transfer, add an allowlist entry, change protected repository policy, or
authorize automated publication. No LLVM private source has been sent to the
proposed carrier as part of this proposal.

## Candidate, fixed scope

- Source: `ores-truffle-oreslang/oreslang-source.llvm`, **PR #988 only**
- Exact source commit for first qualification:
  `65c59c3fb38c889a1cb8b1856e0b140e2c2f8b20`
- Exact source Git tree:
  `447a627f9cd7bc96bc4c61268d117e8565e9a365`
- Proposed destination: **private** `sonus-auris/sonus-auris.infra`
- Destination immutable GitHub repository ID: `1271706990`
- Source-free proof: Actions run
  [40225924937](https://github.com/sonus-auris/sonus-auris.infra/actions/runs/40225924937)
  executed real runner steps and passed.
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
   `native_opaque_unknown_explicitly_unsupported`, then relevant native
   parser, ownership, and security regressions. Preserve complete logs,
   exact compiler revision, exit statuses and job/step receipts.
7. Fail closed on Actions zero-step failures, skipped checks, missing
   tools, parser failures unrelated to the expected diagnostic, stale
   source, or any discrepancy in Git tree SHA.

## Freshness and reconciliation

This proposal tracks LLVM #988's current-main two-parent reconciliation at
`65c59c3fb38c889a1cb8b1856e0b140e2c2f8b20`. The resulting PR diff contains only `Parser.cpp`, the CTest
registrations in `CMakeLists.txt`, and two byte-identical guest fixtures.
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
