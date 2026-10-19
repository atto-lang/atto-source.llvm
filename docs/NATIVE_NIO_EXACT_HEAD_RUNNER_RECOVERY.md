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

## Incremental real-eventfd authority broker (host only)

The draft integration branch now includes
`src/main/cpp/oreslang/runtime/NativeFdAuthorityBroker.hpp` and
`tests/native_fd_authority_broker_test.cpp`. The broker *owns* a real Linux
`NativeEpollReactorManager`, binds `NativeFdAuthority` capabilities to
kernel-created eventfds, and prevents numeric-FD exposure from this API.
Failed admissions do not publish watches. Revocation removes logical
rights before cancellation; only a consumed actual epoll terminal permits
host close and logical `confirm_reclaimed`. A stale/foreign token cannot
authorize a new registration.

The extra fixture is included in both native ownership and VM sanitizer
workflows, with GCC/Clang and sanitizer builds. Its execution is **not
established** until a real source-local runner allocates and performs the
compile/test steps. This narrow broker is not connected to the guest NIO
C ABI and does not authenticate supervisor identity itself; callers must
already be trusted host code. Existing raw socket/pipe watches remain
legacy host-only operations, with their separate provenance restrictions.
This must not be described as complete actor capability or Future/await parity.

## Host completion owner routing and reclamation adversarial coverage

The host-only `NativeFdAuthorityBroker::Completion` now carries the authenticated owner metadata from the sealed per-broker entry alongside the opaque token and terminal result. The **trusted supervisor dispatcher** must route by this owner, never by a caller-supplied owner or raw FD. This does not itself authenticate guest principals or expose an actor ABI. The source-local fixture additionally checks two concurrently watched owners, denial of cross-owner signalling, strict capacity exhaustion, logical-slot reclamation, and stale-token rejection after reissue. Execution of these tests remains unverified until a runner executes real build/test steps.

## Deterministic descriptor lifecycle and contention gate (2026-10-19)

The bounded trusted-host broker now fences `mint_eventfd`, `try_watch`, `signal`, `try_cancel`, and `drain_eventfd` on `stop()`, while leaving trusted terminal-drain, `revoke`, and `confirm_reclaimed` available for cleanup. For real Linux eventfds, a delivered READY completion requires explicit owner-scoped `drain_eventfd` acknowledgment before a fresh readiness watch; the kernel eventfd counter otherwise remains readable and produces a spurious immediately-ready rearm. These are host-only methods, not guest APIs.

`tests/native_fd_authority_broker_state_machine_test.cpp` executes 24 serial orderings of signal/cancel/revoke/stop, an exact-once terminal check, shutdown denial, a two-watch rearm after real eventfd-counter drainage, an injected two-attempt terminal-consume failure, and real ledger-mutex contention against pending terminal delivery and mint admission. The controlled lock/consume hooks exist **only** when the test translation unit defines `ORESLANG_NIO_TEST_HOOKS`; they are not compiled into normal host/runtime builds. Actual native workflows compile and execute this fixture in GCC, Clang ASan/UBSan, and GCC TSan jobs. A committed test is not an executed test; do not qualify until a current-head run has real runner/BUILD/TEST-step evidence.

The broker is still an opt-in trusted C++ host model; it does not implement authenticated guest actor principals, descriptor handoff, generic socket/pipe aliasing, or source-level Future-to-await semantics. No cross-organization private source transfer is permitted without P0 #22/#77 resolution and independent protected approval ledgers.

### Strict dirty-eventfd rearm admission

Broker admission now tracks whether any trusted `signal` successfully incremented the underlying eventfd counter. A subsequent `try_watch` is **denied** until the previously delivered terminal has been consumed **and** the authenticated owner successfully drains the counter. The dirty bit is cleared only after the kernel `read(eventfd)` succeeds. A clean CANCELLED terminal with no successful signal may rearm without a spurious drain. Revocation and host shutdown still block rearm. This prevents a stale eventfd counter from producing repeated immediate READY completions and avoids mistaking cancellation for acknowledgment.

## Atomic authority-to-kernel transactions (2026-10-19)

The host FD broker previously performed kernel registration and *then*
attempted a separate `NativeFdAuthority::begin_watch()` try-lock. A transient
ledger-contention failure could therefore strand an epoll ticket, globally
poison the broker and prevent terminal cleanup. Similarly, closing the
physical eventfd before acquiring a ledger reclamation lock could leave
an already-closed FD in a logically pending state.

The broker now uses private, friend-limited composite ledger transitions:
`try_mint_with`, `try_begin_watch_with` and
`try_confirm_reclaimed_with`. Each acquires the ledger lock *before*
invoking its trusted physical open/watch/close operation. On denial, the
physical operation is **not invoked**; a subsequent invocation may retry
safely. After a successful kernel operation, the authority state is
committed before the lock is released. Ordinary public ledger-only
methods remain separate for host-model conformance tests. The composite
callback must be the trusted manager's narrow synchronous operation,
never guest code, user callbacks, actor state loans or untrusted plugins.

The extra source-local state-machine regression injects two denied
watch admissions, two denied reclamation receipts, a real held ledger
mutex during cleanup, and verifies no phantom epoll registrations,
incorrect physical close, poisoned healthy broker or stale capability
reuse. These cases are in the existing native ownership GCC / Clang
ASan+UBSan / GCC TSan workflow and the native-VM lifecycle sanitizer
script. They are still **NOT_EXECUTED** until a real source-local runner
starts and actual build/test steps complete.

No private-source mirror, cross-organization checkout, or CI policy
authorization was introduced. This remains a trusted-host Linux
eventfd-specific integration, not guest actor ownership or
Future-to-await conformance.
