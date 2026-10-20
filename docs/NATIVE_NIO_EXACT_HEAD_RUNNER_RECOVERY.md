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

## Native manager, C ABI, and VM bridge fail-closed validation (2026-10-19)

Two expanded adversarial fixtures are checked in:
`tests/native_epoll_manager_adversarial_test.cpp` and
`tests/native_abi_parity_adversarial_test.c`. The former exercises the
actual Linux epoll-backed manager: a saturated 64-watch reactor, admission
denial while terminal receipts are pending, strict ticket uniqueness after
retirement, cross-manager raw pipe/dup-alias exclusion, a manager-owned
kernel eventfd through READY, explicit physical counter drain, shutdown
STOPPED terminal, and no duplicate or orphan outcomes. The C fixture uses
the actual compiled native C ABI: rejected raw eventfd and invalid requests,
zero-thread lazy bootstrap, cross-manager public ticket forgery, global raw
stream exclusion, release after consumed terminal, exactly-once cancellation,
and preallocated RT startup.

**Source-level fail-closed defect reproduced**: both
`ores_native_nio_try_pop` and `ores_native_vm_nio_try_pop` previously
returned 0 without clearing an existing `OresNativeNioCompletion`. In
particular, a stale ticket and FD remained in caller-owned output on empty
queue or denied VM lease. The native ABI and VM adapter now reset all
non-null output fields on unsuccessful calls (including null runtime/VM),
as `try_watch` already does. The official VM denial fixture now tests
this behavior 1,000 times, using intentionally unavailable C-VM lifecycle
test doubles.

**Local native validation**: original production broker/reactor/manager
and ABI/VM adapter source blobs were fetched from the private PR and Git
SHA checked in the isolated local workspace. The new C and manager test
source blobs also match their Git object SHAs. GCC 14.2 and Clang 17 each
passed the new real C ABI and manager tests 3/3 with ASan+UBSan. GCC TSan
passed 3/3, and the bridge adapter compiled and passed its original denial
fixture and an additional local real-backend shim fixture 3/3 under those
three sanitizer configurations. The bridge shim is **not** the full C VM
or scheduler. Separately, 20 serial optimized smoke cycles per boundary
fixture passed.

The native ownership workflow now includes 11 native fixtures, and the
VM lifecycle sanitizer script also compiles and runs the new C, manager,
and bridge denial cases. **GitHub-hosted exact-head CI is still NOT_EXECUTED**
unless the run allocates a real runner and performs compile/test steps.
Local file-level exact-blob testing is narrower than full-tree, compiler,
guest actor, and Future-to-await conformance. Neither a private-source
export nor an independently approved cross-organization mirror occurred.

## Native VM bootstrap self-close deadlock prevention (2026-10-19)

Audit finding: `ores_native_vm_close` retired and destroyed scheduler
domains before waiting on `nio_idle` for an unfinished NIO bootstrap.
If the **same** trusted host thread that issued
`ores_native_vm_nio_begin_bootstrap` synchronously called close, the
condition could never become true: only the reservation owner can finish
or cancel it, and that owner is blocked inside close. The API could
deadlock permanently.

The host close path now checks the bootstrap reservation's `pthread_t`
under `control_lock`, **before any scheduler drain/destruction**, and
returns 0 when the closer owns an outstanding reservation. This preserves
the retired-but-allocated VM so the owner can call
`ores_native_vm_nio_cancel_bootstrap` and subsequently retry
`ores_native_vm_close`. A separate external closer still waits for an
in-progress bootstrap; existing cross-thread close/borrow tests are
unchanged. The native VM lifecycle fixture adds 32 deterministic
self-close/cancel/retry iterations. This is a host lifecycle guard,
not guest runtime conformance or a guarantee that unauthorized
close-from-actor calls succeed.

An independent source-free faithful pthread control-lock/condition
model passed 10,000 reentrant close/owner-cancel/retry iterations under
GCC and Clang ASan+UBSan. It is **not** execution of the private C VM
and cannot stand in for exact-head CI or the combined C VM plus actor
scheduler. The production regression is committed to source-local
native VM lifecycle CI and requires a runner to execute.

## Unexpected owned-FD close failure quarantine (2026-10-19)

The host manager's `close_owned_eventfd` now separates authenticated Linux
`close() == 0` or `EINTR` reclamation from an **unknown close result**. The
unknown result invalidates the old handle and *quarantines* that manager slot:
it must not become available for another minted eventfd even though the
underlying numeric FD may already have been released. Never retry the same
numeric FD after a failed Linux `close`, because another thread may have
reused it. The broker's logical reclamation receipt still fails closed and
must not be forged from an ambiguous kernel result. Quarantined slots are
permanently withheld until manager destruction, with operator escalation for
an unexpected close error; capacity reduction is preferable to stale authority.

The source-local native broker state-machine fixture defines
`ORESLANG_NIO_TEST_HOOKS` and injects an ambiguous outcome **after an actual
successful kernel close**, proving that the old capability cannot act and the
slot stays quarantined through an unrelated normal close. These hooks are not
compiled into release builds and do not alter guest APIs. Full native-tree
compilation, the actual fixture, sanitizer results, and actor integration are
**NOT VERIFIED** until a real current-head runner executes BUILD/TEST steps.

## Reentrant carrier close with the actual C VM (2026-10-19)

The native VM lifecycle fixture includes 32 complete native-host NIO
bootstrap, actor spawn/send, callback-triggered close refusal, and external
host teardown cycles. A **real** scheduler carrier callback calls
`ores_native_vm_close`, which must return 0 without joining its own
carrier. The external host observes callback return with bounded polling,
verifies retired VM NIO access remains unavailable with cleared terminal
storage, and then safely completes `ores_native_vm_close`. This fixture
is part of the exact-head source-local C VM+C actor scheduler+C++ NIO
sanitizer script, not a source-free model. It is **not tested** until a
real runner compiles and executes that script; do not infer an Actions PASS
from a commit or source-free model.

## NIO host borrow-owner self-close prevention (2026-10-19)

The C VM could deadlock if a trusted host thread called `ores_native_vm_close` while holding its own `ores_native_vm_nio_acquire` lease: the closer waited on `nio_idle` for a borrow only it could release. The source now uses bounded (16 distinct VM identities) thread-local host lease tracking, rejects cross-thread release, and refuses same-thread close **before** destroying schedulers. It retains foreign-thread close waiting, nested borrow counts and external producer fences. Real native VM lifecycle source tests add 32 close/release/retry iterations. A local harness with the exact original close-function body plus acquisition/release transitions reproduced the original self-deadlock (timeout exit 124); the patched fragment passed GCC/Clang ASan+UBSan three runs each and GCC TSan twice (500 self-close and 100 foreign-close cases per run). Full source-local native VM/scheduler and LLVM CI execution remain required; fragment tests do not establish full-VM parity.
