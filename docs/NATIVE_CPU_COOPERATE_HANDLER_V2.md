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
Shutdown abandons the active logical turn and counts it without invoking the
handler again after stop; an external lifetime owner must join and quiesce
the actor before retiring or freeing its frame.

This deliberately extends the existing **one-actor-per-HungryCarrier**
contract. It is not multi-actor run-queue fairness, not a production
source-level actor handshake, and not hard realtime. The source compiler's
experimental scalar-loop ABI in #308 is still opt-in; its normal fail-closed
loop/rt-cooperate gates remain unchanged.

The AOT integration test compiles a real .ores loop and lets the actual
HungryCarrier worker resume each of 64 independent messages three times
before completion. It verifies single-thread actor execution, mailbox
ordering, no fake IO, stop during indefinite cooperation, and invalid legacy
handler/protocol rejection. It tests -O2 and -O3/-DNDEBUG with live checks.
Future work: general source actor frames on a single multi-actor carrier
(#321), cancellation acknowledgement, pinned-loan admission (#45), and
cross-backend fairness and latency.
