# Native Future readiness / cancellation arbitration

Status: **implemented host continuation-store primitive; NOT guest-source
`async`, `await`, channels, `select` or Task lowering parity**.

## New host protocol

`NativeContinuationStore` uses a bounded, generation-checked AoSoA record
table. For a Future/channel wait that is not complete, the host must use:

1. `publish(... AWAIT ...)`: create a PENDING continuation bound to an
   execution/allocation/code generation.
2. `notify_ready(handle)`: atomically transition PENDING to READY after the
   producer has actually committed a result and the owner has reserved a
   scheduler wake. A second ready signal or a stale handle is rejected.
3. `try_claim_ready_resume(handle)`: exclusively transition READY to CLAIMED;
   a PENDING continuation is *not runnable* through this API.
4. `finish_resume(handle)`: only the successful resume turn may finish.
5. `release(handle)`: only after all scheduler/broker and physical frame users
   have left; generation/slot fencing rejects subsequently stale claims.

`cancel(handle)` competes with either PENDING or READY and wins only until
the scheduler claims execution. Cancellation after CLAIMED is a separate
cooperative Task cancellation responsibility, not a second return/unwind.
All transitions use the existing continuation store mutex and retain
reference-counted ancestry; there is no guest callback on the completion
thread.

The preexisting `try_claim_resume` allows a direct trace bootstrap from
PENDING, and remains for legacy synchronous/host causal-trace callers.
**Never use the legacy direct-claim path for an unresolved Future.**
The source compiler still has to pick the strict API and prove it cannot
accidentally reach that legacy path.

## First compiled-source integration

The restricted compiled `await actor.get_ready()` vertical slice now uses
the strict ready-only resume API in `SourceActorBootstrap.c`.
`complete_ready_if_admitted_locked()` first commits the actual
`NativeActorFuture` and reserves root CONTROL admission, then marks the
persistent continuation READY while holding the source group's mutex.
The root resume turn must acquire that same mutex, so no wake can observe
the Future as complete without observing its continuation READY. The resumed
guest callback receives an already-claimed trace record; the existing
actor-source and host trace tests check the execution lineage and CLAIMED
state. This is a **real compiled Oreslang actor get_ready execution path**,
not merely a disconnected C++ substrate.

The group is still process-local, the codegen only accepts one actor and one
await site, and the top-level trusted launcher may block outside actor
carriers on the final completion condition variable. This does not establish
`Future<T>`, `Task`, arbitrary values across suspension, or guest
`select` support.

## Test

`native_continuation_ready_cancel_resume_race` verifies:

- no ready-claim before producer notification;
- one-shot ready/claim/finish and no double completion;
- early cancellation forbids late wakeup;
- 512 completion-vs-cancel races while PENDING;
- 512 cancel-vs-resume races after READY;
- stale generation denial, bounded store reuse, and clean ancestry retirement.

The CTest builds production `NativeExecutionContextStore.c` and
`NativeContinuationStore.c` with strict C11 warnings and pthread workers.
Run with native ASan/UBSan on an approved CI runner in addition to normal
CTest. A workflow with zero executed steps does not certify it.

## Release blockers

This is a host primitive, not sufficient to declare any of the P0 parity gaps
closed. Before guest source admission, the LLVM code generator must emit typed
continuation frames and bind every awaited operation to a task/actor
generation, source effects, cancellation authority, and mutable-loan fence.
The Future/channel producer must reserve a bounded wake; when wake admission
fails, readiness cannot be silently published without a durable retry path.
The scheduler must authenticate owner generation before the claim, run exactly
one guest continuation on a legitimate actor turn, and join its physical frame
before native heap retirement.

Native `select` additionally needs a single winner across *all* registered
channel/timeout/cancel arms and losing-arm detachment; independently claiming
one continuation per arm is not select correctness. Java/GraalVM ↔ LLVM
source-execution fixtures and exact-head sanitizer evidence remain mandatory.

## Native Task/continuation integration branch (October 24, 2026)

The composed native integration branch brings together the two previously
separate draft components: the source-compiled ActorRef.get_ready() readiness
gate and the runtime-private structured TaskCancellation authority tree.

TaskContinuationGate.hpp is a **new host-only bridge**. It accepts a pinned
OresNativeSuspensionLink and uses the TaskCancellation revocation subscription
to call the generation-checked continuation store's cancel operation.
The Task authority cancels its subtree first, with revokers firing before
ordinary cancellation observers. A pending or READY waiter therefore cannot
be claimed after its revoker has won; the strict scheduler bridge alone can
claim READY -> CLAIMED. Closing the gate cancels unclaimed work and detaches
the subscription before stores can be retired. The binding's mutex protects
borrowed store access during concurrent close and callback invocation.

The independent integration CTest:
- verifies parent/child cancellation and child physical-lifetime observer order;
- verifies cancellation before registration and late after CLAIMED;
- races producer readiness against Task cancellation (256 iterations);
- races root CONTROL continuation claims against Task cancellation (256);
- races teardown/detach against revocation (128);
- verifies fresh resumed logical execution identity, allocation ownership,
  one-shot finish and live-store accounting.

**Explicit remaining limitations:** This host bridge has no guest TaskHandle
representation or lowerable task source syntax, does not atomically couple
scheduler enqueue with Task authority revocation, does not yet own the physical
execution release, and cannot claim that Task get_done() is quiescent. A Task
cancelled after CLAIMED still requires *cooperative* cancellation checks in
guest execution. Native callbacks and C stores remain borrowed with an explicit
close-before-store-release contract. Until an executed exact-head CTest exists,
these tests are committed test definitions, not independently verified passes.

Release gates: run host CTest under ASan/UBSan and optionally TSAN; run
source_actor_await_trace_runtime and source_actor_compiled_native_lifecycle;
verify exact source-tree hash, executor identity, exit codes and test logs.

## Safety audit follow-up: retirement and claimed-frame pins

Two additional fail-closed fixes were implemented after the initial composed
PR was opened:

1. **Retirement is a revocation event** for runtime waiter subscriptions.
   Previously `TaskCancellation::retire()` silently closed all revocation
   subscriptions, so a native continuation bound to a retired Task could
   remain PENDING/READY and be resumed. Retirement now invokes every pending
   revoker outside the graph lock, drains all hooks even when one throws,
   and only then reports a hook failure. A revoker registered after retirement
   is synchronously delivered; an ordinary cancellation *observer* is not.
   This closes retire-vs-registration and retire-vs-producer races.

2. **Final release is forbidden while CLAIMED.** A `NativeContinuationStore`
   record that has been claimed by a resumed physical guest turn cannot have
   its last reference dropped before `finish_resume()` marks COMPLETED.
   Nonfinal pin releases remain permitted. This prevents a stale executing
   continuation handle from being recycled before its owner finishes.
   Existing tests and the new Task bridge test assert the failed premature
   last-release followed by successful finish and cleanup.

The new retirement race adds 128 more iterations. The native host test corpus
now also covers late revocation registration, throwing retirement callbacks,
and stale/forged continuation handles. **These are committed regression
definitions; they are not executed test evidence until CI actually runs.**

### Alternate-organization execution limitation

A separate writable private test repository with a history of executed Actions
steps was identified and individual Git blobs could be copied there with the
same SHA. A full exact-tree mirror was not completed: the source tree contains
over a thousand blobs, the copy process was incomplete, and publishing the
test workflow was blocked by a tooling safety check. No test result from that
partial mirror can qualify this source head. Only an authorized full tree
with identical source tree SHA, or direct checkout in a funded/available
source-org runner, is eligible for the strict exact-head evidence claim.


## Third audit: authority binding and capability separation

`TaskContinuationGate` now requires the caller to supply a trusted
`expected_code_generation` and complete
`OresNativeAllocationDomainRef` (kind/domain/owner/generation) in addition
to the store-backed suspension link. It rejects mismatched code versions,
actor/task heap owners, and any boundary other than AWAIT. This prevents
accidental reuse of a valid generation-checked logical execution from a
different owner or a supervisor CONTROL/spawn/mail boundary.

Negative tests cover forged source execution IDs and handles; a mismatched
code generation; a mismatched actor-owned heap; and CONTROL, SPAWN, and
ACTOR_MESSAGE boundaries. Positive tests pass explicit generation=11 and
the originating ACTOR allocation. A Task authority remains a host-side
capability: the scheduler must derive its expected code/allocation identity
from the trusted suspended execution, never from arbitrary guest input.

The hosted adapter and these tests remain unverified by executed CI.


## Standalone host verification command

An exact checkout can run the three native host test executables without
requiring a complete LLVM/compiler build:

```bash
CC=gcc CXX=g++ bash scripts/test-native-task-continuation-gate.sh normal
CC=gcc CXX=g++ bash scripts/test-native-task-continuation-gate.sh sanitize
```

This compiles the real C11 runtime stores and C++17 Task authority/bridge
test under strict warnings and executes all three host regressions. It does
not substitute for the compiled Oreslang source actor or Java/GraalVM
parity tests. The script's Bash grammar and the corresponding CMake
language-specific compiler flags have been checked locally, but the native
production sources and test executables have **not** been compiled/executed
in this session because the exact Git checkout is unavailable in the
isolated local container and Actions jobs are not admitted.

Use an actual private exact-tree mirror only after verifying the source and
destination `git rev-parse HEAD^{tree}` match. No success receipt may cite a
partial source mirror or any run with zero executed steps.


## Claim-owner authorization and cross-organization native CI attempt

An additional review found that the host Task gate exposed a no-argument
`finish_resume()` method to *any* gate bound to the same underlying
continuation handle. Another binding could therefore finish and release a
CLAIMED frame it had not admitted. The gate now records whether its own
`try_resume()` won the READY-to-CLAIMED transition and refuses
`finish_resume()` from non-claiming gates. This is a runtime-host gate-level
fence, not an exposed guest TaskHandle completion API. Its native regression
tests also check the completion path after `close()` for a legitimately
admitted frame.

The October 24 CI attempt used a private alternate organization:
`streempilot-test/mcp-contract-e2e`, branch
`ci/oreslang-llvm-task-pr935-20261024`, commit
`4f771bd9bb4e67ac7ce73985cb91c6e8c928bbe6`.
The 14 source blobs required for the standalone C11/C++17 host native
test were byte-identical by Git blob SHA to LLVM PR #935 at
`f9e87bed2d93e4b56d0637c6823bef255519f358`.
This was intentionally a **component-exact subset, not a full-tree
mirror**. Workflow run `40132500787` triggered, but its normal and
sanitizer jobs both failed with **zero executed steps**. No native test
was run and no green source result was established. The later head
including the claim-owner fix has not been mirrored or tested.

A separate private candidate, `zed-pkg-test/java-app`, received a
source-free runner-capacity canary on branch
`ci/oreslang-pr935-runner-capacity-20261024`, commit
`2797a6947e334ea0445d3045d092b9401de08e04`; at initial
inspection no corresponding Actions run was visible. It therefore
cannot be treated as proven available capacity.

Before any merge, require a running private CI environment, a complete
exact-tree mirror (including full source-tree SHA equality, not only 14
matching input blobs) for a **full-tree** parity claim, and explicit,
executed regression and sanitizer logs on the final source head.


## Native component test evidence and claimed-ancestor release audit

Executed external component test evidence predates the current claimed-ancestor
fix. GitHub Actions run
[40136151601](https://github.com/StreemPilot/streempilot-e2e/actions/runs/40136151601)
passed both normal and ASan/UBSan jobs against **14 Git-blob-identical source
inputs** at source commit
`3f15978f383896771a3a8c1764871fc4964734ee`, using Ubuntu 24.04
with GCC/G++ 13.3.0. Its output includes
`NATIVE_TASK_CONTINUATION_GATE_PASS`,
`NATIVE_CONTINUATION_READY_CANCEL_RESUME_PASS`, and
`NATIVE_TASK_GATE_TESTS_PASS`. This is not a full-tree checkout or
compiled-language conformance proof.

Subsequent audit discovered a subtle ownership-status bug: releasing a nested
continuation could successfully free the child, then encounter a CLAIMED
ancestor's protected final pin and incorrectly return failure. A caller might
retry the already-released child or misdiagnose the cleanup as incomplete.
The corrected release contract treats the completed child release as success
while retaining the ancestor's protected final execution pin. A new C11
regression verifies the full nested CLAIMED sequence and explicit
finish/release. **The new commit and regression have not yet executed** on
a runner and must not inherit the old green receipt.

### Private-source export governance

Source-main policy currently names **no** approved private CI destinations in
`.github/private-ci-approved-repositories.txt`. Live source `main`
reported `protected=false`, and branch-protection API reads were unavailable.
The policy `docs/security/CI_PRIVATE_SOURCE_MIRRORING.md` expressly says
that direct GitHub connector blob/tree transfers are not an authorization
bypass. No further export is permitted without security/maintainer-reviewed,
protected-main destination approval, pinned carrier contract, and trusted
publisher checks. Prior temporary source-bearing carrier branch refs have
been reset to their clean base; retained Git objects, caches, and Actions
logs may still exist and require security-owner review.

A source-free canary can establish runner capacity but **cannot** authorize
source export. A source-org exact-head runner is the alternative to a fully
authorized private exact-tree transfer.

## Exact suspension-link validation on native completion

The direct native bridge's `ores_native_await_trace_resume_finish()` now
rejects a link unless all of its continuation, parent-handle generation, and
logical execution identity match the live continuation-store record. Previously
it looked only at `link.continuation`, so a forged or stale parent/execution
association could finish another CLAIMED frame. The C11 regression injects a
different parent generation, execution number, and runtime domain while a real
frame is CLAIMED; none may complete it, and the original link must finish once.
The underlying raw C store API remains a **trusted runtime** entrypoint, not an
unforgeable guest capability: arbitrary guest access to native store pointers
or completion calls must remain prohibited.

This hardening is committed on top of the previous externally executed,
14-blob-only normal/ASan/UBSan success. **That earlier green run does not test
these new finish checks, and is not a full-tree provenance receipt.** Source-org
GitHub Actions currently fails admission even for no-checkout canaries. The
protected-source allowlist has zero approved external destinations and `main`
reports `protected=false`; no new cross-organization source export may run
until those existing governance controls have been established and reviewed.
