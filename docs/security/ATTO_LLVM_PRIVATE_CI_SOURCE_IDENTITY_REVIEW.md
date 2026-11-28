# Private LLVM CI source identity migration — review only

## Scope and source binding

- Publisher source repository: atto-lang/atto-source.llvm.
- Exact source repository ID: 1448700962 (verified with connected GitHub repository metadata).
- Historical identity ores-truffle-oreslang/oreslang-source.llvm / 1410897874 is archived; do not use it for new source PR verification.
- Source branch remains main. This draft binds PR head AND base repository names and numeric IDs, adds identity/ABA denial tests, and wires the custody suite into CI governance. Production export remains disabled.
- Legacy Oreslang platform-conformance incident gates, reviewer identities, existing carrier allowlist and test-plan SHA pins remain unchanged. They require independent review/migration before any operation.

## Unconditional export denial remains in place

Production publish() starts with require_atomic_ref_rollback(), which raises Denied unconditionally for unresolved atomic-ref cleanup issue #1154. Source identity and a working destination GITHUB_TOKEN cannot override this. The new test asserts that the migrated identity stays bound and Denied occurs before GitHub API or bounded-gh calls.

## What would be required to activate authorized token-free CI

1. Independent security administrator resolves atomic ref cleanup ABA and protected branch requirements; do not weaken guard for convenience.
2. Independently review any incident migrations, exact destination repository ID, allowlist, workflow blob, test plan SHA and PR number(s). Current carried plans cover only historical PR numbers and do not authorize #834.
3. Establish actual runner capacity with source-free jobs, then mirror only reviewed exact full Git tree to the approved private carrier and verify per-blob SHA, modes and source provenance; no private export until the above are satisfied.
4. Execute CMake/CTest, native guest O0/O2, negative no-IR gates and sanitizers, and record nonzero job steps and exact head hashes. Zero-step action failures do not count as code failures or passes.

**This change does not authorize source export, create carrier branches, dispatch CI, or unblock merges. Tests not yet executed. Keep draft.**

## Atomic ref-custody / ABA remediation plan (#1154)

**Current supported state: DISABLED. No private source export.** The publisher's `require_atomic_ref_rollback()` raises `Denied` as the first operation in `publish()`. `delete_exact_ref()` also denies without a GitHub/CLI request. `_gh_ref_request()` permits GET only. Neither a successful funded runner nor a SHA read-back authorizes REST DELETE by name.

### Threat model and non-solutions

After GET reads ref A, another writer can move the name to B, delete/recreate it, or perform A -> B -> A (ABA), before REST DELETE by name. DELETE success and later 404 cannot establish which incarnation was deleted. GitHub's REST ref DELETE has no documented expected-SHA precondition. A bare `git push --force-with-lease` also does not establish exclusive custody against ref name reuse or ABA without independent protection.

### Requirements before any future activation

1. Independently establish protected source/carrier main, exact status-check identities and app IDs, CODEOWNER rules, no bypass, and verifiable read-back. Private-plan API 403 or unavailable enforcement means DENY.
2. Independently approve source repository Atto ID `1448700962`, exact source/tree SHA, the private carrier repo ID, immutable workflow/test-plan hashes, principals and retention. Legacy Oreslang approvals are not reusable automatically.
3. Independently enforce exclusive write/delete custody for the entire source-bearing ref namespace, including authorization/cleanup refs. A protected, immutable reservation/epoch ledger must reject replay, reuse and ABA; the publisher cannot self-attest exclusivity.
4. Require a documented and executed provider-side atomic compare/delete primitive OR independently enforced exclusive namespace custody and reviewed crash-safe quarantine. Local GET/SHA, guessed `If-Match`, and unprotected REST DELETE are insufficient.
5. On timeout, ambiguous POST/DELETE/404, moved/recreated ref, mismatched repo ID, stale receipt, owner drift or uncertain custody, quarantine the namespace and DENY further handoffs. Escalate independent/manual cleanup; never treat TTL expiry as deletion authority.
6. Execute exact-head adversarial tests against the actual publisher under enforced governance: A->B, A->B->A, 404 and recreation, ambiguous transport, concurrent owner, lost receipt, repeated cleanup, stale approval, and namespace takeover. Require nonzero Actions steps, reviewed logs and SHA evidence.

A future separate independently reviewed PR may replace the unconditional export guard only once these guarantees are proven. **This PR does not implement or authorize that replacement.**

### Tests and evidence boundary

- The publisher now binds both PR head and base repository **names and numeric IDs**; the regression suite covers eight negative identity mutations. Existing source-tree test fixtures have matching IDs to avoid hiding their intended negative paths.
- `scripts/test_private_ci_ref_custody.py` provides eight source-local, no-network tests including guard-first AST structure, denial before I/O, forged environment flags, blocked REST writes, invalid ref names, eight adversarial ref histories across two ref types, repeated cleanup, and an unsafe GET/DELETE model.
- `.github/workflows/private-ci-governance.yml` now triggers on this suite and executes it with the existing V3 publisher regression suite three times if a runner starts.
- Source-free LLVM19 toolchain evidence: [push run 44861170579](https://github.com/scintilla-run-test/atto-ci/actions/runs/44861170579) and [PR run 44861177084](https://github.com/scintilla-run-test/atto-ci/actions/runs/44861177084), both successful. These tested only generated code, not the publisher or private Atto source.

**Source publisher and ref-custody regression tests on this PR are NOT_EXECUTED until real source-runner job evidence exists.** Source-free toolchain capacity is not an export authorization, compiler build or atomic-ref safety proof.
