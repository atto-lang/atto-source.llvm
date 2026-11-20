# Review request: exact-tree native locality CI carrier (2026-10-20)

**Review only. No source transfer and no active authorization changes.**
The destination allowlist remains empty. This review requests security
consideration for PRs #756 (indexed actor ready queues) and #761
(typed GPU queue preflight) in the private native Oreslang repository.

## Proposed destination and immutable plans

- Candidate private repository: `fiducia-cloud-test/java-client-consumer`
- Observed GitHub repository ID: `1323679401`
- Carrier test-plan review: https://github.com/fiducia-cloud-test/java-client-consumer/pull/30
- `ci/test-plans/llvm-pr756.json`: exact-source GCC/Clang native indexed
  ready scheduling, CONTROL-before-DATA and cross-domain physical turn gate.
- `ci/test-plans/llvm-pr761.json`: native DOP GPU queue tenant/device
  mismatch and paired buffer-fence admission; follow-on ASan/UBSan.
- Provider-side authenticated real GPU execution and actual L1/L2/LLC
  counter measurements are separate release gates.

## Required approval order

1. **Protect origin `main` first.** GitHub reported `protected: false`
   on 2026-10-20. Require CODEOWNER reviews, stale-review dismissal,
   enforce admins, prohibit force-push/deletion/review bypass, verify
   effective branch-protection policy. CODEOWNERS alone is not enforcement.
2. Security owners review/merge carrier PR #30 to its private source-free
   `main`; then pin the final exact carrier `main` SHA, workflow Git blob
   SHA and each test-plan blob in the origin reviewed contract.
3. Submit separately for CODEOWNER review the exact destination slug in
   `.github/private-ci-approved-repositories.txt` and matching pinned
   `.github/private-ci-carrier-contracts.json` entries limited to source
   PRs #756 and #761. Do not authorize arbitrary PRs or test plans.
4. Use the repository's audited V3 publisher from protected source
   `main`; verify origin open-PR current head, directory/file modes,
   every blob SHA, and original whole-tree SHA *before* any carrier ref.
   Source transfer must be into private carrier only, with zero cross-org
   source checkout, no public refs/artifacts and no copied secrets.
5. Require carrier receipt, expiring authorization ref, exact mirrored
   tree match, immutable test plan/workflow and populated successful CMake/
   CTest steps. Keep both implementation PRs draft if checks cannot run.

No step above may be shortcut by adding an allowlist line in an unreviewed
branch or treating CI billing failures as executed compiler results.
