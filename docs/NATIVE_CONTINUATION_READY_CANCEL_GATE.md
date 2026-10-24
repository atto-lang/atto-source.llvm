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
