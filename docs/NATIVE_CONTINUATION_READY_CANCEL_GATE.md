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
