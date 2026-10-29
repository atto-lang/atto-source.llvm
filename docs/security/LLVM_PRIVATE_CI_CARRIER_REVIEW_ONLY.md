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
| Source PR head at proposal creation | `0bc88d079d78e15e3abfc946a8eb6f87d078e151` (historical, **not** current authorization) |
| Previously observed candidate actor head (historical; NOT approved) | `40ae15ac5fdfb52f3e83dd16aa5e92eb23832d9e` (superseded; no independent sign-off) |
| Latest observed source PR head (2026-10-29 Git API; NOT approved) | `4fa3306c8a857eef6bbc841fea31460d15e99543` |
| Latest observed source root Git tree (2026-10-29 Git API; NOT approved) | `557bc08d0c78aafb1ec981ee160e10a2f50687ea` |
| Source root Git tree independently approved | **NO — observed identity requires reviewer attestation and live recheck before export** |
| Source `main` protection/ruleset proof | **DENY: GitHub branch API reports `protected:false`, required checks enforcement `off` (2026-10-29); protection endpoint returned 403 to GitHub App, so no additional rule attestation is available** |
| Source main allowlist | **EMPTY — zero active destinations** |
| Security owner | `@ORESoftware` (independent decision required) |
| Approval state | **DENY** |

**Live protected-origin veto (2026-10-29):** authenticated GitHub
`GET /repos/ores-truffle-oreslang/oreslang-source.llvm/branches/main`
reported `protected:false`, `protection.enabled:false`,
`required_status_checks.enforcement_level:"off"`, and an empty required
check list for `main` at `6ca6a6911a6eeedc51e6865f87e8b73a5d604a3e`.
The direct branch-protection endpoint is not accessible to this GitHub App
(`403 Resource not accessible by integration`), and `GET /rulesets`
returned `403` with a plan/feature availability error. These results
**do not prove protected-source enforcement**; the branch's explicit
`protected:false` is sufficient to **deny any private source export**
under the V3 policy. An administrator must establish protection and
independently attest effective source-main enforcement before reviewing a
destination allowlist. This review-only document does not remediate
branch settings, bypass the protected-main prerequisite, or grant approval.

The latest observed head/tree were read from GitHub's immutable git-commit
metadata endpoint, not inferred from filenames or a check-run. They establish
the identity of that particular snapshot only; they are NOT an independent
security approval, destination approval, signed publisher permit, runtime
test result or authorization to transfer source. An authorized publisher
must re-read the current PR head and exact root tree under an approved
protected-source policy and reject drift before any API write.

The source PR SHA above is only a review snapshot. If the PR changes, the
source head, root tree, file inventory, and reviewed test plan must be rebound,
re-reviewed, and attested again. A matching file list alone does not imply an
identical Git tree. Treat mismatched root tree, blob, mode, commit, visibility or
repository ID as a hard stop. The candidate test plan MUST NOT be loaded from a
PR-controlled checkout as if it were the authority for its own export.

### Runner admission re-probe and retained-source incidents (2026-10-29)

On governance branch head `847cbac570f9dfb663cb8fe418bfaf59fe3ead15`,
source-org source-free [run 40822470456, attempt 2](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/actions/runs/40822470456/attempts/2)
was **explicitly rerun** using GitHub Actions permissions. Its `runner-admission`
job `123280013756` returned `runner_id:0`, `steps:[]`,
`conclusion:failure`, with one check-run annotation. That annotation
cannot be read through the available GitHub App endpoint; an administrator
must inspect the job/check UI and report its literal error before inferring
billing exhaustion or deciding any runner policy change. Logs were not
available. Source-free status is an admission measurement only, not a
compiler/actor test. See [runner issue #159](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/issues/159).

Independent [source retention incident #1150](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/issues/1150)
is **open** and documents pre-existing cross-org private LLVM source-bearing
test refs and incomplete Git-object/Actions retention disposition. Ref
resets do not prove private blobs were purged. This independently vetoes
fresh transfers until the security owner resolves incident disposition
and confirms authorized retention, destination ACLs and full export gates.
No source-bearing data was copied to investigate or reproduce the issue
during this admission re-probe.

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

### Fresh alternate-organization source-free runner result (attempt 2)

A repeat of the unrelated private `honeypot-r-us-test/hnpt-infra`
[source-free run 40813194544, attempt 2](https://github.com/honeypot-r-us-test/hnpt-infra/actions/runs/40813194544/attempts/2)
executed successfully with six recorded steps on job `123288048929`:
setup, admitted runner/toolchain inventory, standalone GCC/Clang C11 toy
test, standalone GCC/Clang C++17 threaded toy test, Clang ASan/UBSan toy
test and completion. This confirms *fresh source-free toolchain capacity*,
not a verified Actions billing balance or an approved source-bearing carrier.

The latest LLVM actor PR #1126 source-local hardening now includes explicit
native runtime dependency path filters, clean-checkout assertions, pre-run
Git blob verification for native authority/turn-gate/source actor tracing
and 48 additional concurrent wake-publication versus target-STOP attempts.
[Exact-head source run 40825645119](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/actions/runs/40825645119)
again failed the `actor-ledger-contract` and `compiled-ores-guest`
jobs with **zero** executed steps. Thus there is no native compiler or
sanitizer verdict for this source tree. No private LLVM source was copied
into the unrelated runner repository during this canary.

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

## 7. Administrator decision and evidence runbook (source-free)

### A. Diagnose source-org runner admission first

An org/repository Actions administrator should inspect the GitHub Actions
job annotations and corresponding billing/runner entitlement and policy,
and record the *actual GitHub error string* in
[LLVM #159](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/issues/159).
A job that completes with no runner and `steps=[]` does not identify whether
the cause is spending cap, Actions policy, private-repository plan allowance,
runner-group restrictions, or another admission constraint.

1. Inspect exact-head actor run
   [#40817865185](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/actions/runs/40817865185):
   `actor-ledger-contract` job `123263107747` and
   `compiled-ores-guest` job `123263108064` each ended before any steps.
2. Compare the independent source-free gate
   [#40814139647](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/actions/runs/40814139647)
   (job `123251144798`, no checkout/secrets or source compilation).
3. Validate current organization/repository Actions permissions, budget and
   spending limits, availability of `ubuntu-24.04` runners, and any runner
   group, allowlisted-action or branch-workflow restrictions. Obtain the
   original job/check-run annotation rather than inferring "minutes exhausted."
4. Rerun a **source-free**, no-credentials, no-artifacts canary only after
   correcting the diagnosed policy/capacity blocker. Record a positive
   runner assignment, nonempty executed step list, actual job log and
   run-attempt ID. A green synthetic test is not yet LLVM application evidence.

### B. Independent approval for an alternate private carrier

A security owner independent of the actor PR author must record, in a
protected source control plane, the exact destination full name, numeric
repository ID, owner approval, private visibility, GitHub branch-protection
proof, immutable carrier base/workflow/test-plan commit or blob identities,
per-run source/export scope, source retention risk, and disposition of all
open source-transfer security incidents. Approval **must not** be inferred
from the user's request, a previous source-free probe or an author-authored
PR comment. No source-bearing GitHub connector writes before that authority
is both reviewed and effective.

The current funded source-free comparison, private
`honeypot-r-us-test/hnpt-infra`
[run #40813194544](https://github.com/honeypot-r-us-test/hnpt-infra/actions/runs/40813194544),
executed 6/6 synthetic GCC/Clang C/C++/sanitizer steps; it is an unrelated
repository and is **NOT** an authorized Oreslang carrier. Prefer a new
dedicated private carrier with restricted membership.

### C. Immutable execution evidence to request

For each *authorized* run, preserve a non-source receipt containing:

- Original immutable source repository ID, current PR number/head commit,
  original root-tree Git SHA and source manifest digest (not only file names)
- Independently approved destination repository ID, private visibility,
  trusted control-plane commit, workflow blob and test-plan digest
- Carrier source-subtree Git-tree read-back proving exact identity before
  executable refs, followed by approved receipt and cleanup/retention proof
- Actual Actions run ID, run attempt, job IDs, runner assignment, executed
  steps, compiler versions, GCC/Clang/native and Clang ASan/UBSan verdicts
- Real `oreslang-llvmc` guest compile and host bridge WAKE/STOP results,
  independent physical actor-heap isolation/loan teardown, and rejection
  cases with a terminal and mailbox accounting matrix

A passing test on a **different** source head or a job with zero steps
cannot be reused. The workflow's compiler-path CI gap is now statically
repaired on actor head `40ae15ac5fdfb52f3e83dd16aa5e92eb23832d9e`:
the gate has `main.cpp`, `frontend/**`, `types/**`, `backend/**`
triggers and immutable Git-blob checks for all eight compiler translation
units. [LLVM #1151](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/issues/1151)
still requires **executed** compiler-only trigger acceptance; source-level
read-back by itself is not a substitute.

All three stages remain **DENIED or UNVERIFIED** as appropriate until
their independent receipts are recorded. This runbook is documentation,
not an executable authorization gate.

**Decision requested from independent security/organization administrators:**
restore source-organization runner admission, OR establish a dedicated private
carrier with live protected control-plane proof and immutable repository ID
plus explicit source-export authorization. No transfer or merge is permitted
on the strength of this proposal alone.
