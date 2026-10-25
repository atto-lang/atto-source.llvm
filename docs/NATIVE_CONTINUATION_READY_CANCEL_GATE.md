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

## Actor-owned NIO wake handoff

The native VM now has an **opt-in**, ABI-separate wake-capable constructor.
The original `OresNativeVMConfig` layout and `ores_native_vm_open()`
semantics remain unchanged; hosts that need continuation wake escrow supply
`OresNativeVMWakeConfig` to `ores_native_vm_open_with_wakes()`.

For an actor-owned one-shot readiness wait, the VM-NIO bridge uses this
physical ownership sequence:

1. reserve one generation-fenced scheduler wake for the exact producer actor;
2. retain one exact ActorRef external-resource loan;
3. admit the bounded epoll watch;
4. consume READY/CANCELLED/STOPPED into the bounded actor NIO claim;
5. TRANSFER the scheduler wake, which is the irreversible ownership handoff;
6. release/free the NIO resource and FD claim, then drop the NIO claim mutex;
7. PUBLISH READY/CANCELLED/FAILED so the continuation becomes runnable only
   after the old FD claim is gone.

A TRANSFERRED wake itself fences physical actor DONE, so step 6 introduces no
lifetime gap. Publishing after the NIO claim is released is required: the actor
wake may run immediately on another carrier and must be able to re-arm the same
FD without racing the previous one-shot claim.

If STOP wins before TRANSFER, transfer fails, the still-RESERVED wake is
cancelled, the NIO resource loan is released, and no actor wake callback runs.
If STOP wins after TRANSFER, scheduler publication consumes the transferred
wake as `PRODUCER_RETIRED` without executing guest code. VM close joins the
reactor first, cancels or aborts every remaining await wake, releases the NIO
loans, and only then drains schedulers.

This is **runtime suspension framing**, not source-level async I/O parity.
Actor source handlers remain synchronous and the compiler still rejects general
`async`/`suspends` I/O lowering. A later compiler slice must allocate and
authenticate the continuation descriptors/code generations supplied to this
bridge before a standard-library I/O operation may be admitted from Oreslang
source.

## First compiled source I/O suspension

The native frontend now admits one deliberately narrow Linux-native source
operation:

```oreslang
await readable(<integer-literal-fd>);
```

This is compiler-owned await syntax, not a user function call and not general
stdlib I/O. Admission is restricted to exported zero-argument `main` with
exactly one live source actor, no suspended scalar/class/array locals, one
suspension site, and a native-int literal descriptor. General `async`,
`suspends`, Future<T>, channels, select, socket/file wrappers and actor-method
suspension remain fail-closed.

The compiler emits a pointer-free scalar continuation with no lexical user
frame. Post-await actor sends/stops are the only effects admitted in this
first slice. Ordinary calls, field writes, defers or other non-actor statements
after the await are rejected until general continuation-frame lowering can
preserve their source order; they must never be silently hoisted before
readiness.

`SourceActorBootstrap` first persists the existing root-actor
`OresNativeSuspensionLink`, then passes its continuation slot/generation plus
the source-offset descriptor generation to
`ores_native_vm_nio_try_watch_actor_await()`. The source group opts into the
ABI-separate wake-capable VM and attaches NIO while the VM is still actor-cold;
throughput reactor threads remain cold until the actual watch is admitted.

On readiness, trusted outer host glue consumes the reactor completion and calls
`ores_native_vm_nio_dispatch_actor_await()`. The #994 handoff transfers the
scheduler wake, releases the old one-shot NIO/FD claim, then publishes the wake.
Only the authenticated source-root actor callback marks the persisted
continuation READY, claims a fresh execution context, and invokes the compiled
continuation. Reactor/completion threads never execute guest code.

A direct Linux runtime gate also arms a real nonblocking pipe before a delayed
writer makes it readable. That independently exercises VM-NIO wake escrow,
logical trace resumption and actor cleanup without relying on compiler IR.

The native E2E fixture uses nonblocking stdin fd 0 and supplies one byte from
the harness. It checks compiler IR, real mixed C/C++ runtime linkage, exact
continuation/wake receipt metadata, actor-lane resume, post-await message
delivery, physical actor stop/reclamation, and both release and ASan/UBSan
variants. Negative compiler fixtures reject nonliteral descriptors, suspended
locals, a second suspension and missing actor ownership.

This is still not a public portable I/O API. The literal-fd form exists to
prove compiler → continuation → VM-NIO → scheduler → compiled-resume ownership
end to end before typed nonblocking stream/socket abstractions are admitted.

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
