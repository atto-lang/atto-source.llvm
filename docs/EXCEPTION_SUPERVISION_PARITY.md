# Oreslang exceptional effects and actor supervision (parity contract)

Status: **normative design and tested host-side primitives; NOT complete guest
compiler parity.** Java/Graal currently implements synchronous `trap`,
`try/catch/finally` and `defer`, but source `throw`, `raise`, `panic` and
`recover` statements are reserved until the typed lowering is implemented.
LLVM currently compiles a strict i64 subset; all of these exception statements
must fail closed instead of producing misleading native code.

## Control effects

| Effect | Caught by catch | Converted by trap | Handled by recover | Unhandled actor outcome |
|---|---|---|---|---|
| throw(E) | yes, typed | `None` | not needed | `on_throw` |
| raise(E) | no | no | yes, explicit | `on_raise` |
| panic(PanicInfo) | no | no | restricted recovery | `on_panic` |
| cancel | no | no | no | `on_cancel` |
| fatal host failure | no | no | no | `on_host_failure` when containable |

These are **effects**, not synonyms and not Java/C++ exception subclasses
exposed to Oreslang. Guest `catch` must not catch scheduler cancellation,
`panic`, JVM `Error`, native fatal signals or process-exit control. `trap`
returns `Option<T>` for synchronous calls and eventually
`Future<Option<T>>` for async calls; it catches **only** uncaught ordinary
`throw`. `Option<Option<T>>` must not flatten. Existing detailed trap
contract: `docs/TRAP.md` in Java.

## Native compiler exceptional CFG milestone (experimental)

The LLVM frontend now admits one local synchronous `int` throw inside a
terminal `try`, where a matching `catch` always consumes the thrown scalar.
Its nonthrowing class-field defers run in LIFO order on dedicated LLVM
`throw.cleanup` -> `throw.catch` edges, and the existing `finally` path
runs after the catch-return snapshot. No general source exceptional-effect
propagation, arbitrary cleanup throws, async unwind or supervisor hook
lowering is implemented. Java's production parser still rejects the explicit
`throw` statement; this is **not cross-backend parity**.

## Bootstrap catch versus trap admission (Java/GraalVM PR #781)

Java distinguishes *explicit recovery* from *silent conversion to None*.
Wrapped and suppressed causes receive bounded fail-closed classification.

| Runtime failure origin | Explicit guest catch | Callable trap |
| --- | --- | --- |
| Ordinary recoverable throw/assertion | Yes | Yes (`None`) |
| Denied filesystem/HTTP permission | Yes | No |
| Recoverable operation timeout | Yes | No |
| Panic/raise, cancellation, interruption, rejected scheduling | No | No |
| Process exit, host-fatal error, unknown control | No | No |

Handling a permission denial does not grant the denied side effect.
The native `GuestFailureBoundary` and `GuestFailureOrigin` admission
helper in `ExceptionPolicy.hpp` is a tested host control-plane contract,
**not compiled LLVM guest `try/catch` or `trap` support**. Unsupported
guest exception source forms must continue to fail closed.

`defer` is lexical and LIFO; `finally` always runs on normal/exceptional
unwind **while the runtime remains executable**. Neither can be promised after
SIGKILL, native UB, OOM exhaustion or externally killed isolates. Under
simultaneous failures, the stronger original effect wins:
`fatal/termination > panic > raise > throw > return`.
A weaker cleanup failure is attached as secondary context rather than replacing
a stronger failure. An ordinary cleanup throw may replace an ordinary return.
Cleanup operations cannot extend a borrow past its lifetime or resume a dead
actor.

A `recover` is an explicit, restricted effect boundary, not an implicit
`catch all`. The compiler must require recovery capability for panic and
validate actor state restoration before resumption. No resumable panic is
claimed in either backend yet. Host panics should not be translated to normal
Oreslang `throw` automatically.

## Typed supervisor protocol

An actor's unhandled failure *ends that actor's execution domain*, preserving
its original classification. No guest exception is injected up the parent's
call stack. The actor lifecycle future settles with the original cause; an
independent supervisor CONTROL event is delivered to its parent/supervisor.

Proposed source hooks (not yet supported by the compiler):

```ores
define actor Parent as
    on_throw(ActorFailure<ThrowError> failure): SupervisorAction {
        return SupervisorAction.restart;
    }
    on_raise(ActorFailure<RaiseError> failure): SupervisorAction {
        return SupervisorAction.stop;
    }
    on_panic(ActorFailure<PanicInfo> failure): SupervisorAction {
        return SupervisorAction.stop;
    }
    on_cancel(ActorCancellation failure): SupervisorAction {
        return SupervisorAction.stop;
    }
    on_host_failure(HostFailure failure): SupervisorAction {
        return SupervisorAction.escalate;
    }
end
```

Use singular `on_throw` (optionally consider an alias `on_throws` later).
Hooks operate on **data-only typed envelopes**, not live exception objects or
mutable child memory. Compiler must check exhaustive coverage for supervised
child failure effects and install a default policy only when the owner
explicitly selects one. A parent with no handler and no configured policy
must fail compilation for supervised child spawns. Recovery policy includes
restart budgets/backoff and escalation to grandaddy/root; an uncaught
supervisor failure must not recursively tear down the hosting process.

The supervisor itself has one logical execution lane (as do daddy/grandaddy).
Separate actors can execute concurrently. CONTROL handlers are admitted
between user turns, never concurrently with the parent's own user code.
If the parent is executing a noncooperative turn, delivery cannot be
instantaneous: bounded safepoint/preemption latency is a separate runtime
release gate.

## Dedicated priority CONTROL lane

The host primitives `SupervisorFailureLane` in Java and C++ deliberately use
a separate queue from ordinary actor mail. Each child reserves **one bounded
terminal-event slot before spawn**; admission denies new children when the
supervision budget is full. Each actor generation may publish one terminal
failure; duplicates are rejected/deduplicated and stale generations fail
closed. Delivery is in flight until explicit acknowledgement, and handler
exceptions requeue the event. Host handler interfaces have five required
methods with no no-op defaults.

There is no fairness claim yet without scheduler integration. The scheduler
must poll the CONTROL lane before every user-mail admission, with an explicit
bounded CONTROL quantum so control floods do not starve all normal mail.
Per-parent priority lanes must not share a lock with any child user mailbox.
CONTROL delivery must not wait for or block on full normal mailboxes.
The event payload is capped to 512 characters in these host prototypes.
A preallocation/critical OOM strategy and guaranteed wake-up are additional
release requirements; a bounded queue alone cannot guarantee delivery when
the process cannot allocate memory.

Suggested lanes: (1) urgent supervisor terminal failures/cancel, (2) bounded
runtime continuations/timeouts, (3) ordinary user mail. The CONTROL/continuation
distinction matters; Java's existing reserved continuation slots do **not**
provide supervisor failure delivery.

## Required parity tests before release

1. Sync and async `throw` caught or converted to `None`; `raise` and
   `panic` bypass ordinary catch/trap and keep type identity after await.
2. LIFO defer/finally through normal return, throw, raise, panic, await,
   cancellation, and cleanup throwing a weaker/stronger effect.
3. Actor panic after startup, during receive, inside await continuation,
   and during final cleanup: parent/root survives, child `get_done()` fails,
   exactly one typed CONTROL event received, no leaked ownership.
4. Saturate ordinary mail then trigger failure: CONTROL arrives at the next
   scheduler admission; verify race, duplicate, stale generation, retry,
   bounded memory and multi-supervisor isolation.
5. Parent handler missing an effect fails **source compilation**; typed
   `on_throw/on_raise/on_panic` dispatch, escalation/restart budgets and
   supervisor-of-supervisor handling succeed in both backends.
6. Host VM-fatal/native crash behavior is tested with physical isolates or
   separate processes; do **not** promise survival from in-process UB or OOM.

## Current implementation limits

- Java/Graal: `ActorRuntime` now reserves a terminal-failure CONTROL slot
  **before actor publication** and publishes terminal failures before resolving
  `get_done()`. Host/root supervisors can `pollSupervisionFailure()`, then
  `acknowledgeSupervisionFailure(...)` or `retrySupervisionFailure(...)`.
  The typed record carries the child UUID/generation and immediate parent UUID.
  Guest panic markers use `GuestFailureSignal` rather than fragile JVM
  exception-class-name matching. Cancellation and unclassified host exceptions
  stay distinct. Successful actor exit releases its reserved slot.
  **Not yet wired**: parent actor guest `on_*` hooks, compulsory compiler
  coverage, automatic supervisor scheduler admission and restart strategies.
- JVM/Graal GC: explicit actor FFM arenas govern eligible native allocations,
  but Java Throwable and metadata objects still use managed memory. Do not
  claim GC-free Java execution. An untrusted actor must be placed in a true
  isolate/process for physical fatal-failure containment.
- LLVM: the `SupervisorFailureLane` queues preallocated intrusive CONTROL
  nodes with bounded inline summary/parent metadata. `publish_preallocated`
  does not grow a queue or allocate event data. Separately,
  `EffectUnwind.hpp` offers stack-allocated, fixed-capacity typed cleanup
  statuses (`Return < Throw < Raise < Panic < Cancel < Fatal`) with LIFO
  `defer/finally` execution and a bounded secondary failure log. Compiled
  guest exceptional effects, native actor scheduler admission, await-aware
  unwinding and source `on_*` hooks are **not implemented**.
- Java/Graal source evaluator cleanup now runs all defers in LIFO order after
  intermediate cleanup failure, merges synchronous and suspended cleanup
  failures without downgrading stronger control effects, and applies the same
  precedence when running `finally`. Targeted GraalVM 25 tests: **18 passed,
  0 failed, 0 errors, 0 skipped** at
  https://github.com/zed-pkg-test/java-app/actions/runs/38096013585.
  Further nested async/cancellation/fatal-failure model checks are required.
- Both parsers reserve unsupported effect statements and fail closed.
- Funded SHA-pinned host-component and targeted Java/Graal Maven evidence:
  https://github.com/zed-pkg-test/java-app/actions/runs/38095853057
  and https://github.com/zed-pkg-test/java-app/actions/runs/38095841027
- These changes improve actor-failure containment, but do **not** establish
  end-to-end parent priority scheduling, compiler-enforced guest handlers or
  crash-proof operation for a host VM/native process.


## Parent CONTROL admission + source contract (October 10 follow-up)

The Java/Graal runtime now reserves **two independent bounded slots at child
admission**: a parent-local urgent CONTROL slot and a root escalation slot.
The child finalizer completes the child readiness/done barriers, then enqueues
a failure on the parent slot and schedules the parent's existing single-flight
actor lane. That lane polls failures before continuations and user mail, even
when an ordinary `receive` is suspended. The child finalizer never invokes
guest handlers. Parent `ACKNOWLEDGE` retires the unused root reservation;
`ESCALATE`, parent shutdown or handler failure publish to the root lane.
A stopped parent also drains orphaned queued failures to root.

Host `SupervisionHandler` covers all five typed terminal effect kinds;
`onRestarted(failure, replacement)` is optional. Host `RESTART` launches a
new actor with a fresh UUID and fresh initialization, **never** reuses the
failed actor's state/arena or replays its mailbox. Each parent has at most
three restart attempts. Exhaustion or spawn failure escalates to root.
The new reference is passed to the host hook on the parent's own lane.

For guest Oreslang actor classes, the TypeChecker rejects partial
`on_throw/on_raise/on_panic` hook sets, enforces private synchronous
`(String summary): void` method signatures, or accepts explicit
`supervision_policy(): String`. The evaluator invokes typed hook names on
its own actor turn, or consults the policy for `"escalate"` and
`"acknowledge"`. Guest actors without either contract may remain leaf
actors but are denied nested actor spawn at runtime. Guest-language
`"restart"` policy, first-class typed Failure payload and guest
`on_restarted` ActorRef lowering are not yet supported; restart is currently
host/native-ABI only.

`ActorGroup` remains a separate cooperation domain. It is **not** the
supervision parent, and its Mailman is not allowed to borrow an actor's
single-flight execution authority.

The LLVM branch has `SupervisorControlTurn.hpp`: a native runtime admission
ABI for typed CONTROL events, checked single-flight lease, restart budget,
escalation and retry. Its unit tests prove priority over ordinary mail, but
the native LLVM source compiler still rejects actor/exception language
features that cannot be lowered, and has no integrated production actor
scheduler. These substrate tests do not prove complete backend parity.


## October 10 next-gate hardening (funded exact-source proof)

- **LLVM lost readiness race closed.** The native `SupervisorControlTurn::urgent()` no longer reads a mutable cached atomic readiness bit. Previously a consumer could store `false` after a concurrent producer stored `true`, leaving queued CONTROL without visible readiness. Readiness now derives from the synchronized queue's pending count. Exact native subset CI passed:
  https://github.com/zed-pkg-test/java-app/actions/runs/38099624188
- **JVM/Graal compile-time admission tightened.** For direct nested source `spawn`, the TypeChecker rejects actor code without an inherited or locally declared complete `on_throw/on_raise/on_panic` handler set, or an explicit `supervision_policy()`. Runtime admission checks remain authoritative for dynamic invocations.
- **Transactional restart cleanup.** If `onRestarted()` rejects a freshly spawned actor, the parent revokes/stops that replacement and escalates the original failure instead of leaking an unclaimed child.
- **Broader compatibility proof:** SHA-exact funded GraalVM 25 mirror with 115 tests, 115 passed, zero failures/errors/skips. Covers actor runtime, source actors, ActorGroup Mailmen, continuation/cooperative Mailmen, terminal event queue, restart and parser contracts:
  https://github.com/zed-pkg-test/java-app/actions/runs/38099928020

### Remaining high-priority integration sequence

1. Implement proper **supervisor-of-supervisor** escalation. Current JVM routing delivers to an immediate parent, then escalates to the runtime-root reserved lane; it does **not** offer that descendant fault to each intervening grandparent. Design quota-reserved per-ancestor escalation envelopes before admitting descendants, without unbounded fan-out.
2. Integrate LLVM native `SupervisorControlTurn` with emitted actor spawn/turn/finalization and generated typed effect handlers. The existing LLVM compiler does not yet lower these guest actor forms into the substrate.
3. Finish source lowering of `throw`, `raise`, `panic`, `recover` and effect propagation across await on both backends. The reservation/error diagnostics are intentionally fail-closed today.
4. Make the source guest failure payload a first-class immutable typed value rather than only the bounded `String summary`, including parent identity, effect kind, source trace and dedup/generation.
5. Test scheduler wakeup under concurrent stop, cancel, retransmission and suspend; then run full current-head Java Maven, LLVM CMake/CTest, Graal native AOT/hybrid, fault injection, physical isolation, and independent review. Targeted funded CI is **not** release certification.


## October 10: hierarchical escalation and reserved-slot queue hardening

**JVM/Graal actual actor routing:** a descendant now pre-reserves one CONTROL
event slot in every ancestor on successful spawn admission, as well as the
runtime-root slot. A failure is offered to its direct parent first. On
`ESCALATE`, an unhandled/stopped parent passes the **same typed terminal
event** to the next living ancestor's already-reserved inbox, waking that
ancestor's existing single-flight actor scheduler. Grandparent guest handlers
never execute on a descendant carrier. `ACKNOWLEDGE` retires all unused
higher-ancestor and root reservations. Graceful child retirement rolls back
all unused reservations; partial failed spawn admission does likewise.

A three-level real JVM actor integration test confirms grandparent admission
and no duplicate root event. The Java `SupervisorFailureLane` queue is now
an intrusive FIFO linked by pre-reserved per-child slots; publication does
not allocate queue nodes. Concurrent 4-producer/256-event publication and
FIFO retry-order tests passed.

**Latest exact-source funded GraalVM 25 gate:** 118/118 passed; zero failures,
errors or skips: https://github.com/zed-pkg-test/java-app/actions/runs/38103048914

**LLVM typed native contract:** an exact-source C++17 test exercises child
publish → middle supervisor CONTROL admission → grandparent CONTROL
admission → typed `on_panic` ACK, with independent native logical turn leases.
It does not imply compiler-generated native actor code is available.
Exact-blob funded LLVM CI passed:
https://github.com/zed-pkg-test/java-app/actions/runs/38102983344

**Release gates still unresolved:** native LLVM source actor scheduling/IR
lowering, full guest throw/raise/panic/recover and typed failure objects,
handling of concurrent stop/cancel while pending ancestor events are in
flight, complete Maven/CMake/CTest/native-image runs, profiler/GC and
physical isolation benchmarks, and independent review. Source PRs remain
draft and unmerged. Do not represent targeted subset CI as full release
certification.


### Additional stop-vs-failure proof

A dedicated JVM/Graal test stages a terminal child PANIC while its middle
supervisor holds a user turn; an external host stops the middle supervisor.
Its reserved pending CONTROL event is flushed to the grandparent's logical
lane and ACKed without root duplication. Latest funded exact-source GraalVM
25 suite: **119 passed, 0 failures/errors/skips**:
https://github.com/zed-pkg-test/java-app/actions/runs/38103218616

This exercises a deliberate stop/turn overlap, but does not certify every
adversarial force-kill, concurrent cancellation, or independent native
scheduler race. No LLVM source-level actor codegen parity is claimed.


## October 10: real native carrier and inherited restart validation

**JVM/Graal source/runtime:** restart-candidate immutable factory metadata now
follows an escalated descendant event into the next live supervisor. A parent
handler failure/retry retains ownership, and stopping supervisors transfer the
candidate with queued failure metadata before clearing local references.
Grandparent `RESTART` creates a new actor identity and fresh state, not a
revival of the previously finalized child actor. Source policy remains
explicit `escalate/acknowledge`; guest-source restart syntax is still not
lowered.

**Latest funded exact-source GraalVM 25:** 120/120 tests passed, zero
failures, errors or skips:
https://github.com/zed-pkg-test/java-app/actions/runs/38106610733

**Native LLVM runtime integration:** `HungryCarrier` accepts an optional
externally owned `SupervisorControlTurn` and typed `Hooks` pair at
construction. Its lifetime-gated `ProducerHandle::try_wake_control()` notifies
the parent's actual native carrier only after a pre-reserved event is
published. The native actor loop admits pending CONTROL before ordinary mail
or pending I/O completion. A typed `EffectUnwind` primary effect can be
published directly with `EffectSupervisorBridge::publish_terminal`.
The new two-carrier test proves the parent's `on_panic` runs on its own
carrier, ahead of queued user mail; never on the child's thread. Handles
reject new sends/control wakes after shutdown.

**Native funded exact-source validation:** C++17 strict -Werror build and
execution, 30 repeated native carrier integration runs, followed by an
ASan/UBSan instrumented compile/execution all passed:
https://github.com/zed-pkg-test/java-app/actions/runs/38106747752

These improvements are **not LLVM guest actor codegen**. The current LLVM
front-end still rejects Oreslang actor declarations and guest effects it
cannot lower. `HungryCarrier` currently has a dedicated native carrier
thread rather than the final shared continuation pool. Complete actor-lane
scheduling, automatic CONTROL redirection upon native parent shutdown,
cross-await typed effect propagation, and source-driven actor lifecycle
remain release blockers. Full Maven/CMake/CTest/AOT and independent review
remain outstanding, so both PRs stay **draft/unmerged**.
