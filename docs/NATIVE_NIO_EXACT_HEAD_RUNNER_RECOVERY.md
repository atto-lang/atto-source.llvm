# Native NIO source-local exact-head CI recovery

Status: draft security and runtime evidence runbook. This document is **not**
authorization to export private source, not evidence of an executed test, and
not an assertion that the guest NIO bridge implements NativeFdAuthority.

## Current blocker and permitted recovery

The Native NIO descriptor-ownership, native VM lifecycle sanitizer, and
full LLVM workflow jobs were observed in the October 18/19, 2026 audit
with no allocated runner, no started_at, and **zero steps**. The GitHub
API reported failure/skipped, but those statuses do not demonstrate a
compiler/test failure. The cause (Actions minutes, billing, runner
availability, queue policy, or another platform condition) has not been
independently established.

1. A repository/organization administrator must inspect the **source
   organization's** Actions usage, spending limits, runner availability,
   workflow permissions, and job scheduling diagnostics.
2. Restore a suitable source-local runner inside the original private
   source repository/organization. An isolated, authenticated, single-use
   source-local runner requires independent security approval, appropriate
   runner-group restrictions, and verified teardown. A workflow label is
   not proof of isolation. Do not automatically turn on any unapproved
   source-local self-hosted runner.
3. Re-run the current source-head workflows, not a historical PR merge
   artifact. Read the PR head *again* immediately before evaluation.
4. Keep external mirrors and cross-organization checkout/export **disabled**.
   Independently reviewed protected source/destination/snapshot ledgers,
   separate approvals and P0 incident #22/#77 resolution are prerequisites
   for any future private-source transfer. Spare Actions minutes in another
   organization are not authorization.

## Immutable source identity

Read the live current head and the canonical Git tree, rather than
trusting a hard-coded runbook SHA:

```bash
repo=ores-truffle-oreslang/oreslang-source.llvm
source_head="$(gh api "repos/$repo/pulls/638" --jq '.head.sha')"
source_tree="$(gh api "repos/$repo/git/commits/$source_head" --jq '.tree.sha')"
printf 'PR=638 HEAD=%s TREE=%s\\n' "$source_head" "$source_tree"
```

The checkout steps of both workflows target the pull request's source
`head.sha` and verify worktree integrity with
`scripts/verify-native-nio-source-local.py`. The verification
script detects local Git fileMode/skip-worktree/assume-unchanged
masking and raw tracked-byte changes; a bare `git status` is not
sufficient. A successful run still needs exact-tree evidence.

## Minimum *executed* evidence

For a candidate workflow run:

```bash
run_id=REPLACE_WITH_CURRENT_HEAD_RUN_ID
gh api "repos/$repo/actions/runs/$run_id"
gh api "repos/$repo/actions/runs/$run_id/jobs?per_page=100" --jq '
  .jobs[] | {
    id, name, status, conclusion, runner_name, runner_id, started_at,
    completed_at, steps: [.steps[]? | {number,name,status,conclusion,started_at,completed_at}]
  }'
```

Confirm all of the following using GitHub responses, retained logs,
and, when present, source-local artifacts:

- Re-read the PR head; the source commit tested is still the **current**
  source head, and the observed checkout commit/root tree equal the
  immutable source commit/tree above.
- The workflow's source repo ID and run event/branch identify the
  intended PR or immutable dispatch. A successful unrelated merge-ref,
  old source SHA, or carrier run does not qualify.
- A real runner was assigned to the relevant job, and `started_at`
  and positive build/test step execution timestamps are populated.
- The compiled source has a successful exact-head/worktree verification
  step before native compilation, and the build and test steps themselves
  were **actually executed** to completion. A skipped dependency job or
  green policy/preflight alone is insufficient.
- Native descriptor ownership:
  `Compile manager and C ABI against actual private source files` **and**
  `Execute native, bridge-denial and host-capability-model tests three times`.
  The matrix must distinguish GCC, Clang ASan/UBSan and GCC TSan results.
- Native VM lifecycle:
  `source-integrity` and `native-vm-nio` jobs, including
  `Compile real C VM, actor scheduler and C++ NIO under both sanitizers`.
  The script also separately executes the FD-authority host model with
  GCC/Clang ASan/UBSan; this is **not** proof of guest FD authority.
- Report precise GitHub workflow URL, run ID/attempt, source repo/PR,
  source commit SHA/tree, job ID/runner identity, executed test steps,
  outcome, sanitized diagnostics and any artifacts. Never count failed
  pre-allocation or zero-step jobs as compiler/test failures.

## Runtime integration boundary

`NativeFdAuthority` is a bounded host-only ownership ledger.
`confirm_reclaimed` must only be called by the trusted broker **after**
all transport aliases are fenced, kernel watches are retired, and
terminal delivery has drained. The ledger itself does not close or verify
OS descriptors and does not accept arbitrary guest numeric FDs.
The native VM bridge has not yet been proven to enforce this model.

A passing standalone FD-authority test or sanitizer fixture establishes
host-model behavior only. Cross-backend Future-to-await, actor physical
turn admission, heap placement, reactor shutdown and guest NIO
conformance remain separate release gates.

## Stop conditions

Stop and retain draft status if any runner/job lacks executed steps,
head/tree changes, unexpected workspace/index state appears, a native
sanitizer fails, or an independent source-transfer security gate is
unapproved. Never equate absence of runner capacity with runtime parity.
