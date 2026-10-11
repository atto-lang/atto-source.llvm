# Oreslang exception effects and actor supervision (cross-backend contract)

**Status: incremental, not feature parity.** This contract applies to both
`oreslang-source.java` and `oreslang-source.llvm`. The table below distinguishes
desired language semantics from features the current compilers actually lower.

## Exceptional control effects

| Effect | Semantics | Ordinary `catch` | `trap` |
| --- | --- | --- | --- |
| `throw e` | A recoverable, typed error | Yes | Converts uncaught throw to `None` |
| `raise e` | An explicitly escalated, non-trappable failure | No | Bypasses |
| `panic e` | An invariant failure, non-trappable in ordinary source | No | Bypasses |
| cancellation / actor termination | Scheduler lifecycle control | No | Bypasses |
| VM fatal error | Host/runtime cannot guarantee continued execution | No | Bypasses |

* `try/catch/finally` catches only the ordinary `throw` channel. A matching
  typed `recover` boundary, when implemented, may explicitly consume an
  escalation or panic; ordinary `catch` must never accidentally do so.
* `trap fnc f(): T` returns `Option<T>`: normal exit is `Some(result)`,
  unhandled ordinary throw is `None`. `Option<T>` results are not flattened.
  Async trap requires `Future<Option<T>>` with the boundary spanning **every**
  suspension; until implemented it is rejected.
* A panic thrown by `Option.unwrap` / `Result.unwrap` is a true panic, not a
  recoverable ordinary throw.
* `defer` executes in reverse registration order, once, on normal and
  exceptional exit. `finally` executes once after a matching or bypassed catch.
  Cleanup must not turn cancellation, raise or panic into success.
* Failure precedence (strongest first): VM-fatal/process termination,
  cancellation, panic, raise, ordinary throw, normal control flow. Lower-priority
  cleanup failures must be retained as suppressed diagnostics. All registered
  defers must be attempted, even when a preceding defer fails.
* `recover` is **not** a general-purpose replacement for `catch (Throwable)`.
  Proposed admission: only a supervisor/recovery scope with explicit typed
  capabilities may consume `raise` or `panic`, and it must leave a recorded
  event. It cannot recover VM corruption or forcibly killed isolates.

## Actor failure / containment

An unhandled `throw`, `raise` or `panic` terminates its actor's active
invocation. Default supervision is fail-stop for that actor and, under the
existing structured-child policy, cancellation of its descendants. Unrelated
actors and the application's main/root context remain runnable. A severe
`VirtualMachineError`, `ThreadDeath` or `LinkageError` is **not** guaranteed
to be isolated from the host process. No language-level handling can make OOM,
native memory corruption, OS kills or process-wide VM failures safe.

A supervisor must be able to hear *failure control events* separately from
ordinary actor data, including when its normal mailbox is at capacity.
A second **CONTROL/supervision lane** is not merely a higher-priority item
in the data FIFO:

1. Use one sticky, exactly-once terminal notice per failed actor; expose the
   notice through a runtime-owned, non-cancellable future/capability.
2. Deliver notices without acquiring a regular mailbox slot, waiting on a
   full data channel, or executing supervisor guest code inline on the
   failing actor's carrier.
3. Supervisor scheduling needs reserved capacity and a bound on work per tick.
   CONTROL callbacks must not create an unbounded bypass/DoS vector.
4. Parent/supervisor lifecycle ownership, supervision policy and actor IDs
   are checked; untrusted actors cannot forge escalation notices.
5. A notice is immutable **data only** (actor identity, category, diagnostic),
   not a cross-heap `Throwable` or an actor-owned mutable reference.
   Notice diagnostics are capped at **1024 UTF-8 bytes**, without splitting
   Unicode scalar boundaries. Native invalid UTF-8 input is replaced with '?'.
   This cap applies on direct record construction as well as emitted notices.
6. Fairness means ordinary user traffic still progresses; urgent events
   cannot indefinitely postpone data processing and data cannot starve control.
   Scheduler starvation from a non-cooperating busy loop is a separate concern.
7. A normal stop / structured cancellation is **not** an invented panic.

### Proposed source-level required handlers (not yet implemented)

For a supervised child spawn or ActorGroup member, the compiler should
require exhaustive coverage of terminal failure kinds. Recommended canonical
handler names are `on_throw`, `on_raise` and `on_panic`, with explicit typed
parameters and `SupervisionDecision` results. A supervisory `match` on a sealed
`ActorFailure` union is an equivalent alternative. `on_throws` can be considered
a compatibility alias, but must not be a second, ambiguous contract.

For example, **design sketch only, not currently accepted source**:

```oreslang
define actor Parent as
  on_throw(Failure<Throw> error): SupervisionDecision { return Stop; }
  on_raise(Failure<Raise> error): SupervisionDecision { return Escalate; }
  on_panic(Failure<Panic> error): SupervisionDecision { return Restart; }
end
```

The compiler must reject a supervised relationship missing any required
branch **or** demand an explicit exhaustive forwarding policy. It should not
require every *unrelated* actor to declare three empty methods. Repeated
restart needs rate limits, backoff, and exhaustion behavior; no automatic
unbounded retry. Handler invocation is serialized within supervisor state,
does not unblock the failed actor, and cannot suppress the audit event.

## Implementation status (2026-10-10)

**Java/GraalVM**

* Existing `defer`, `try/catch/finally`, sync `trap`, actor fail-stop and
  per-actor `ready/done` observability are implemented to varying degrees.
* `OresPanic` is a distinct runtime marker; `OresRaised` is now reserved as
  a host runtime marker, but source `raise` grammar/lowering is **not** implemented.
* Suspension-aware catch must explicitly bypass panic and escalation as the
  synchronous path does; a regression fixture covers the distinction.
* `ActorRef.failureNotice()` is a **host/runtime control-plane primitive**:
  a single immutable, non-cancellable terminal signal independent of user
  mailbox traffic. It is not yet a compiler-checked actor source callback.
* The direct-parent `ActorRef.nextChildFailure()` inbox is now a separate
  bounded CONTROL queue with one pending reader and no user-mail dependency.
  Parent queue overflow fails closed; host/parent observation is supported.
* Actor class methods spelled `on_throw`, `on_throws`, `on_raise`,
  `on_panic` or `on_recover` currently fail closed in the Java parser; an
  ordinary user method must not masquerade as a checked supervisor handler.
* ActorGroup-level supervision dispatch, CONTROL scheduling reservation,
  checked `on_*` handlers, source `throw/raise/panic/recover` lowering,
  exhaustive effect checking and full cross-suspension cleanup audit remain
  future implementation work.

**LLVM native**

* The current compiler only lowers the restricted i64 function/return fragment.
  No general exceptions, actor execution, cleanup unwinding, `trap`,
  `recover` or supervision handlers exist yet.
* Reserved exceptional keywords and unsupported control statements must
  produce a nonzero compilation exit and **no LLVM IR**. This is a
  fail-closed compatibility safeguard, not implementation parity.
* A tested native `ExceptionPolicy.hpp` value-only policy primitive now
  mirrors Java precedence (host/process-fatal > cancellation > panic >
  raise > throw), suppressed diagnostic retention, and the 1024-byte
  Unicode-safe notice limit. This is **not** integrated with source lowering
  or native actors; it does not count as behavioral feature parity.
* LLVM parity still requires native unwind/cleanup lowering, ownership-safe
  error payloads, actor-private heaps, supervisor/control queue scheduling,
  fatal isolation and exact same observable traces as Java.

## Required differential regression matrix

Cover each effect before and after `await`, `select`, `do select`,
`nb select`, I/O suspension and actor boundaries. Include:
`throw` caught vs trapped; panic/raise bypassing catch and trap; nested
`defer` LIFO; cleanup failures while another failure is in flight;
`finally` on bypass; typed supervisor decisions; parent mailbox at
capacity; priority fairness; panicked worker with a healthy sibling;
startup, handler and final-cleanup failure; duplicate/racing failures;
cancellation vs panic; untrusted-actor isolation and fatal-host escalation.

Do **not** mark either backend feature complete merely because a parser
accepts a spelling or a host helper class exists. Positive Java/LLVM runtime
conformance and negative rejection tests must both be green on their exact PR
heads.
