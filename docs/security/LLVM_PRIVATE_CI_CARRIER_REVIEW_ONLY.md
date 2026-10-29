# Review-only request: authorized private carrier for LLVM actor PR #1126

**Status: PROPOSAL ONLY — NOT AN AUTHORIZATION TO COPY SOURCE.**

This document is an independently reviewable security and test-plan handoff. It
does not add an entry to `.github/private-ci-approved-repositories.txt`, create
a source-bearing Git object in another organization, install a privileged
workflow, approve an export, or alter the requirements for PR #1126. The
allowlist deliberately remains **empty** until protected-source governance and
an independently approved carrier have been established.

## 1. Source identity and immutable evidence

| Item | Reviewed candidate value |
| --- | --- |
| Source repository | `ores-truffle-oreslang/oreslang-source.llvm` |
| GitHub source numeric repo ID | `1410897874` (verify live again before every write) |
| Visibility | Private (verify live before every write) |
| Source PR | `#1126`, draft until executed CI and independent review |
| Source PR head at proposal creation | `0bc88d079d78e15e3abfc946a8eb6f87d078e151` |
| Source root Git tree | **UNVERIFIED — must retrieve authoritatively before approval** |
| Source `main` protection/ruleset proof | **UNVERIFIED — independently verify effective enforcement** |
| Source main allowlist | **EMPTY — zero active destinations** |
| Security owner | `@ORESoftware` (independent decision required) |
| Approval state | **DENY** |

The source PR SHA above is only a review snapshot. If the PR changes, the
source head, root tree, file inventory, and reviewed test plan must be rebound,
re-reviewed, and attested again. A matching file list alone does not imply an
identical Git tree. Treat mismatched root tree, blob, mode, commit, visibility or
repository ID as a hard stop. The candidate test plan MUST NOT be loaded from a
PR-controlled checkout as if it were the authority for its own export.

## 2. Known source-free capacity (NOT an approved source carrier)

- Private `honeypot-r-us-test/hnpt-infra`, GitHub numeric repo ID
  `1343208978`, is an **unrelated project**, not an approved Oreslang carrier.
- [Source-free run 40812979195](https://github.com/honeypot-r-us-test/hnpt-infra/actions/runs/40812979195)
  completed successfully with five actual recorded steps: native GCC/Clang C11,
  Clang ASan/UBSan, toolchain inventory. No Oreslang source was present.
- [Source-free run 40813194544](https://github.com/honeypot-r-us-test/hnpt-infra/actions/runs/40813194544)
  completed successfully with six actual recorded steps, additionally proving
  GCC and Clang C++17 threading compilation and execution.
- The temporary [source-free draft PR #14](https://github.com/honeypot-r-us-test/hnpt-infra/pull/14)
  must NOT be merged or converted into a source-bearing test branch merely
  because the harmless probe succeeded.
- Small, stepful source-free jobs establish *some* Actions scheduling
  capacity; they do not prove remaining billable minutes for a full LLVM
  compile, runner isolation, repository owner consent, export approval, or
  sufficient cleanup controls.

## 3. Destination authorization — all required before ANY source write

- [ ] Provision a **dedicated private Oreslang CI carrier repository** with
      separate ACLs from existing projects. Do not reuse `hnpt-infra` or
      another unrelated private project without independently documented
      project-owner/security approval.
- [ ] Identify exact immutable destination numeric GitHub repository ID and
      exact `owner/repo` slug. Independently verify private visibility, no
      public fork/copy path, collaborator/Actions permissions, billing, runner
      group, artifact/cache retention and legal/organizational export policy.
- [ ] Establish and verify the carrier control plane on protected `main`,
      including pinned workflow, source-free base commit/root tree, exact
      test-plan blob, source checkout isolation, least-privilege read-only
      execution, and separate cleanup authority. Attest source-free origin of
      the carrier base.
- [ ] Require effective **protected source `main`** with CODEOWNER security
      review, required executed checks, no unauthorized bypass/force/deletion,
      and independently verified GitHub protection/ruleset configuration. A
      draft PR approval or successful source-free canary is not a substitute.
- [ ] Resolve/independently disposition source-private-CI incident holds and
      receive `@ORESoftware`'s exact-head approval of destination, workflow,
      plan, source policy and accepted risk of potentially retained Git objects.
- [ ] Only **after** the above, review and merge a new exact destination
      allowlist/contract on protected source `main`, including BOTH
      `owner/repo` and immutable destination numeric ID, plus pinned carrier
      control-plane/test-plan identities. The current LLVM allowlist format
      names only owner/repo; amend the policy/parser safely and require tests
      before enabling any active destination.
- [ ] Verify every active approval from an independently authenticated,
      protected policy source immediately before each source-bearing API write;
      reject caller-controlled env/workflow inputs as approval.

All checkboxes begin unchecked. This file never grants permission to perform a
cross-org transfer, even when they have been manually edited; the trusted
publisher must validate current authoritative policy and live GitHub state.

## 4. Approved-transfer implementation contract (future, not executed)

After approval, pin the **current** open PR head and original root tree. Reject
symlinks, submodules, special files, unsafe paths, unexpected executable modes,
oversized content, dirty local source, secret-bearing files and unbounded trees.
Create private Git blobs only into the exact reviewed destination numeric
repository ID. Build a **fresh** subtree and compare every path, mode, blob SHA
and complete Git root-tree SHA to the pinned source. Do not silently add
`node_modules`, credentials, artifacts, caches or other unreviewed bytes.

Run a vetted source-free carrier workflow from the independently reviewed
carrier base, never an untrusted PR-controlled workflow. Build the source in a
gitless, credential-free, network-restricted and least-privilege sandbox with
no access to unrelated carrier objects. Record a non-source provenance receipt
linking immutable source PR/commit/tree, carrier repo ID/base/workflow/test plan,
execution job IDs, exact result and any violations. Restrict retention; delete
temporary refs when verified, but never claim ref deletion erases Git objects.

The source repository's `GITHUB_TOKEN` must never be expected to checkout a
private repository across organizations. GitHub connector writes also count as
source transfers and require the **same** independent approval.

## 5. LLVM PR #1126 exact-head execution acceptance

Execute on the original source organization when runner admission is restored,
or on the **approved** private carrier only after the above authorization.

1. Require a real runner assignment and at least one executed step with
   checkable logs; `runner_id=0`, missing logs or `steps=[]` are
   infrastructure evidence only.
2. Revalidate source and carrier identity and the exact original Git commit,
   root tree, immutable manifest and source blobs BEFORE running tests.
3. Execute the source-local `actor-ledger-contract` gate with GCC and Clang,
   exact-head scheduler tests, source actor trace, retained-DATA mailbox/STOP,
   deterministic STOP-vs-WAKE races, actor-owned physical heap misuses,
   cross-suspension loan/pinning, and Clang ASan/UBSan.
4. Execute the independent `compiled-ores-guest` gate: build
   `oreslang-llvmc` from the exact reviewed source, compile real `.ores`
   `guest_step`, `guest_resume`, and `guest_cleanup` functions, verify
   their generated LLVM symbols, and execute the native host-bridge binary
   under release and sanitizers. Confirm normal WAKE and STOP cleanup, B's
   progress after A's physical heap reclamation, counter identities and
   strict actor-owned lifetime rules.
5. Retain the negative compiler check that native `suspends on_message`
   remains rejected. This bridge is NOT compiler-generated actor await,
   generated drop glue, proof of hard-RT WCET, or JVM heap isolation.
6. Require all required test jobs to **actually execute and pass** on the
   identical immutable source tree. Resolve every compiler/sanitizer failure,
   obtain independent review, and only then consider moving #1126 out of
   draft and merging through branch protections.

## 6. Current known blockers and administrator handoff

- LLVM native exact-head [run 40813096713](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/actions/runs/40813096713)
  failed both ledger and compiled guest jobs with **zero** steps. Do not
  classify this as a source regression or a successful compiler test.
- Fresh **source-free** admission probe on this review PR:
  [LLVM source-run #40814139647](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/actions/runs/40814139647),
  job `123251144798`: **failure before execution, zero recorded steps,
  no assigned runner**. This workflow has `permissions: {}`, requires no
  checkout, does not load/compile the private LLVM repository, uses no
  secrets, and uploads nothing. The parallel ordinary LLVM compiler
  [run #40814139498](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/actions/runs/40814139498),
  job `123251143511`, likewise recorded **zero steps**. This separates
  admission failure from C/C++/LLVM compiler correctness, but cannot identify
  the exact GitHub billing/policy/runner restriction without admin evidence.
- [LLVM runner issue #159](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/issues/159)
  requires an organization admin to inspect Actions billing/spending and
  actual runner-admission annotations and fix admission, not repeat reruns.
- [Java private-CI security issue #962](https://github.com/ores-truffle-oreslang/oreslang-source.java/issues/962)
  and [approval handoff #1501](https://github.com/ores-truffle-oreslang/oreslang-source.java/issues/1501)
  describe deny-by-default cross-org export and protected-source conditions.
  The canonical [private source policy](https://github.com/ores-truffle-oreslang/oreslang-source.java/blob/main/docs/security/CI_PRIVATE_SOURCE_MIRRORING.md)
  explicitly denies transfers until independently authenticated approvals
  exist.

**Decision requested from independent security/organization administrators:**
restore source-organization runner admission, OR establish a dedicated private
carrier with live protected control-plane proof and immutable repository ID
plus explicit source-export authorization. No transfer or merge is permitted
on the strength of this proposal alone.
