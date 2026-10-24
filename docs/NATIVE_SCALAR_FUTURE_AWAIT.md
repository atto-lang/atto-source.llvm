# Native scalar Future completion frame — implementation slice

**Status: host-only prerequisite; guest `Future<T>`, `async fnc`, and `await` remain compiler-rejected.**

The `NativeScalarFutureAwait` frame carries one signed 64-bit scalar value or
a positive failure code. It is pinned in a shared allocation, qualified by a
four-part `(runtime_id, actor_id, generation, future_id)` ticket. Competing
completion, failure and cancellation transitions have exactly one winner.
A producer thread may settle the frame but may **never execute a guest
continuation**. Only the owning `NativeActorGroup` parent CONTROL lane may
consume the terminal outcome. When CONTROL is full, the outcome remains ready
for retry. An explicit cancellation after enqueue suppresses the queued guest
callback. If group close drops a reserved CONTROL callback, the generation-
fenced `NativeActorRef::control_delivery_lost()` signal can be consumed via
`cancel_if_control_lost()`, preventing the frame from remaining permanently
scheduled. The actor supervisor still owns that teardown action; this isolated
primitive does not autonomously subscribe to actor lifecycle events.

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

The `native_scalar_future_single_winner_control` CTest covers stale
generation, foreign owner, exactly-once completion, fail/cancel race,
CONTROL backpressure, revocation before execution, and no inline resume.
The acceptance path to guest parity still requires:

1. Typed source `Future<T>` and `async` admission with distinct guest ABIs.
2. Heap/arena-owned resumable MIR frames and compiler-proved ownership.
3. Actor-safe cancellation/terminal kind mapping and child-frame retention.
4. Real source-generated `await` and scheduler resumption on another carrier.
5. Exact-head LLVM and Java differential tests with instrumented scheduling.

The executable host-only test must not be counted as a native source-level
Future/await parity pass.
