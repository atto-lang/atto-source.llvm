# Native CPU cooperation admission — HungryCarrier V2

The legacy ActorHandler callback receives a bool that means **IO completed**.
It is never reinterpreted to mean CPU resume. A new ActorHandlerV2 takes
ResumeCause::initial, io or cpu and may return ActorStep{StepKind::cooperate,0,0}.
An old callback returning cooperate is a protocol failure (CONTROL stop).

The CPU result does **not** mark the message completed, queue a fake broker IO,
overwrite the completion payload, or allow a later user message to overtake the
unfinished turn. The same frame and envelope are resumed as ResumeCause::cpu
on a subsequent physical dispatch, with urgent supervisor CONTROL checked
first. The owning actor state and frame remain local to one carrier thread.
Invalid nonzero cooperation value/delay and unsupported status fail closed.
Shutdown never resumes the ordinary handler after stop. A V2 continuation
may instead install an explicit `ActorCancelHandler`; if a logical turn entered
user code but did not return DONE, the actor carrier invokes that callback
exactly once before publishing `stopped`. The callback runs on the same actor
carrier with `HungryCarrier::active_heap()` authority, so compiler-generated
frame cancellation/drop glue can release actor-owned resources without exposing
them to the broker thread. Queued envelopes that never entered user code do not
receive cancellation callbacks. DONE racing with STOP wins completion and does
not receive cancellation. Telemetry separates callback invocation, callback-reported failure and
active turns abandoned without any callback. The callback returns an explicit
cleanup acknowledgment. If an abandoned turn has no callback or cleanup returns
false, the carrier permanently withholds `CarrierRetirementProof`; joined
threads alone are insufficient authority to reclaim an externally referenced
actor heap. An external lifetime owner must still join and quiesce the actor
before retiring or freeing its frame.

This deliberately extends the existing **one-actor-per-HungryCarrier**
contract. It is not multi-actor run-queue fairness, not a production
source-level actor handshake, and not hard realtime. The source compiler's
experimental scalar-loop ABI in #308 is still opt-in; its normal fail-closed
loop/rt-cooperate gates remain unchanged.

The AOT integration test compiles a real .ores loop and lets the actual
HungryCarrier worker resume each of 64 independent messages three times
before completion. It verifies single-thread actor execution, mailbox
ordering, no fake IO, stop during indefinite cooperation, generated-frame
cancellation, and invalid legacy handler/protocol rejection. The compiled
scalar frame exports `oreslang_rt_scalar_frame_cancel`; the carrier integration
checks that stop moves the active frame to CANCELLED exactly once. Separate
native tests cover STOP-vs-DONE and invalid-protocol cleanup races. It tests
-O2 and -O3/-DNDEBUG with live checks. Future work: owned fields inside real
source actor frames, pinned-loan admission (#45), explicit cancellation/drop
receipts, and cross-backend fairness and latency.


## LLVM codegen-side ownership revalidation

`parse_program()` invokes the native source ownership checker, but
`emit_verified_llvm_ir()` is also callable directly with an in-memory
`Program`. A generated/transformed AST must not be treated as trusted
merely because the caller skips the parser. Before emitting any LLVM IR,
the emitter repeats `validate_i64_ownership()`. The specialized scalar
continuation path first fail-closes on malformed frame structure, then
runs that same ownership check before lowering.

The standalone scalar frame still supports **only** its single initialized
stack-scalar counter, a direct return of that counter, a bounded scalar
loop, and no classes, extra locals, calls, actor effects, borrowed
parameters, exception path, mixed functions or target-specific devices.
It is NOT permission for general actor-owned objects to survive an
`await` or `select`. Those source forms still require pinned-frame loans,
per-field ownership flags and verified cancellation/drop glue.

`native_llvm_emission_checks_ownership_and_frame_layout` is the direct
LLVM-emitter API regression: it constructs source-bypassing `Program`
instances, including use-after-move, extra owners, null initializer,
mismatched initialization and extra functions, and tests a valid program
and valid scalar frame as positive controls. A successful source-org
CTEST/LLVM-linked run is required before crediting this test as executed.
