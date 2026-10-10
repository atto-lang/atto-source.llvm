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

- Java/Graal: the new host-side typed queue is independently testable, but is
  not yet wired into `ActorRuntime`, `ActorGroup` or guest hook compilation.
  Existing actor fail-stop and lifecycle completion remain separate.
- LLVM: header-only host-side queue and CTest; no native source-level
  exception unwinding, actor scheduler admission or guest hooks.
- Both parsers reserve unsupported effect statements and fail closed.
- These prototypes do **not** establish the user's final no-main-crash or
  priority-delivery guarantees until host integration and exact-head CI pass.
