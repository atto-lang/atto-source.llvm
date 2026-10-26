# Native scalar Future completion frame — implementation slice

**Status: host-only prerequisite; guest `Future<T>`, `async fnc`, and `await` remain compiler-rejected.**

The `NativeScalarFutureAwait` frame carries one signed 64-bit scalar value or
a positive failure code. It is pinned in a shared allocation and carries four
numeric routing/debug labels `(runtime_id, actor_id, generation, future_id)`
plus an opaque per-frame capability. The numeric tuple is deliberately not
authority: two simultaneously live frames may reuse the same `future_id`
without accepting each other's settle/cancel/schedule tickets. Competing
completion, failure and cancellation transitions have exactly one winner.
The frame state is atomically arbitrated: `pending -> settling -> ready ->
scheduled -> running -> completed`, with `cancelled` terminal from pending,
ready, or scheduled. Detach racing producer settlement uses the explicit
`settling_cancelled` intermediate state on that same atomic; no second
cross-atomic cancellation flag participates in the linearization. The
transient `settling` state gives one producer sole write authority for the
result before READY is published with release semantics. A scheduled callback claims RUNNING with acquire/release semantics,
so the actor carrier never needs a Future mutex. Cancellation is rejected once
RUNNING begins, and the frame becomes COMPLETED only after callback exit
(including exceptional exit).
A producer thread may settle the frame but may **never execute a guest
continuation**. Scheduling and teardown authenticate the exact shared
`ActorRef` ticket capability in addition to numeric runtime/actor/generation
identity, so even accidental same-generation reuse cannot let a stale ref
schedule or revoke a newer Future. Only the owning `NativeActorGroup` parent
CONTROL lane may consume the terminal outcome. Future phase arbitration is
lock-free: the frame atomically publishes a logical scheduled reservation,
then attempts the bounded ActorGroup CONTROL enqueue. Failed admission rolls
the reservation back to READY unless cancellation won concurrently. This
prevents an immediately dequeued CONTROL turn from parking a carrier behind
Future bookkeeping. When CONTROL is full, the outcome remains READY for retry.
An explicit cancellation after enqueue suppresses the queued guest callback.

Group close publishes the generation-fenced
`NativeActorRef::delivery_closed()` barrier. Per-actor physical finalization
seals `NativeActorRef::resource_admission_closed()` only by atomically
transitioning the actor resource count from zero to its terminal bit.
`NativeScalarFutureAwait::open()` now participates in that same arbitration:
after fast fence checks it acquires one exact-incarnation actor resource loan,
and that CAS-backed loan is the frame-admission linearization point against
physical finalization. A finalizer that seals first makes the loan acquisition
fail closed; an open that acquires the loan first forces finalization to retry
until the frame completes, cancels, or is destroyed. The frame releases its
loan exactly once on cancellation, callback completion (including exceptional
exit), settle-vs-detach cancellation, failed terminal scheduling admission, or
last-reference destruction. A close racing after successful frame creation
still uses the generation-fenced detach reconciliation path; unlike physical
finalization, group close does not need to seal the resource counter.
`try_schedule()` treats either
`delivery_closed()` or `resource_admission_closed()` as authoritative.
With the resource-loan contract, a correctly live PENDING, READY, or SCHEDULED
frame prevents physical finalization from publishing the terminal resource seal
until that frame releases its loan; the seal therefore cannot legitimately
overtake such a frame. The resource-seal check remains fail-closed
defense-in-depth, while `delivery_closed()` is the ordinary close/detach race
signal. If either terminal fence is observed during CONTROL admission, a
still-SCHEDULED frame is cancelled before scheduling reports success whenever
the phase still permits cancellation.
The supervisor's `cancel_if_owner_detached()` hook remains required for a
close that happens after a successful scheduling call has already returned,
and it also covers the transient SETTLING race. `control_delivery_lost()`
remains a diagnostic for an actually dropped reserved CONTROL item, not the
authority for revoking every Future.

This is **not** a zero-allocation result, a full compiler-owned typed MIR
continuation, a safe borrow-across-await implementation, a transport/I/O
reactor, or native guest Future support. No actor heap reference, borrowed
pointer, source closure, or runtime task crosses this host-only frame.
A producer must retain a strong or weak frame reference until its callbacks
have quiesced; frame lifetime and actor group lifetime remain separate.
The scalar **result payload** carries no actor heap pointers. The scheduler-provided
`Resume` closure is not statically inspected for captured pointers: only the
source compiler's future ownership checking can guarantee borrow-safe capture.
This host-only API must never be interpreted as permission to suspend with
an unpinned actor-owned borrow or to execute a callback on the producer thread.

The `native_scalar_future_single_winner_control` CTest also asserts that live
frames block physical DONE, terminal/drop paths release their actor resource
loan, and open-vs-finalize has exactly one CAS-backed admission winner. It
covers stale generation, foreign owner, duplicate numeric Future-ID capability isolation,
post-detach creation rejection, physical-resource admission sealing,
open-vs-finalize races, exactly-once completion, fail/cancel race, CONTROL
backpressure, revocation before execution, and no inline resume.
The acceptance path to guest parity still requires:

1. Typed source `Future<T>` and `async` admission with distinct guest ABIs.
2. Heap/arena-owned resumable MIR frames and compiler-proved ownership.
3. Actor-safe cancellation/terminal kind mapping and child-frame retention.
4. Real source-generated `await` and scheduler resumption on another carrier.
5. Exact-head LLVM and Java differential tests with instrumented scheduling.

The executable host-only test must not be counted as a native source-level
Future/await parity pass.
