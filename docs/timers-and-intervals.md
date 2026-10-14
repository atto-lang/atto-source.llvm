# Oreslang timers, sleeps, intervals and deadlines — cross-backend contract

Status: **proposed source API** with initial host-runtime primitives (2026-10-14).
These snippets are **target syntax**, not evidence that both compilers accept them.

## Public API (proposed)

| API | Semantics |
| --- | --- |
| `time.after(duration) -> Future<void>` | Cancellable one-shot monotonic deadline. |
| `time.sleep(duration) -> Future<void>` | Alias for `after` for `await` composition. |
| `time.sleep_blocking(duration) -> void` | Legal only in a `suspends`-capable frame. The logical task waits but the carrier **never parks**. |
| `time.interval(period) -> Interval` | Owned demand-driven recurring timer; `await interval.next()` returns an increasing tick index. |
| `time.set_timeout(duration, callback) -> TimerHandle` | Schedules a one-shot guest callback on the **owning scheduler**, never on the host clock thread. |
| `time.set_interval(period, callback) -> TimerHandle` | Repeats after the previous callback is finished (fixed delay, never overlaps itself). |
| `time.timeout(duration, future) -> Future<T>` | Races a monotonic deadline against an awaited operation, settles exactly once. |
| `timer.cancel() -> bool` | Idempotently revokes a still-pending timer; returns whether this call won. |

The lexical `250ms` and `1s` literals below are a proposed `Duration` surface;
implement their parser/type checker and canonical formatter consistently before
advertising these examples as runnable. Negative durations are invalid; an
interval period must be strictly positive. Zero-delay one-shot timers are
allowed and still resume on a **later** scheduler turn.

## Async / await, recursion, iteration, and suspends

```oreslang
pub async fnc one_shot(): void {
    await time.sleep(250ms);
}

pub async fnc bounded_recursive(int remaining): void {
    if remaining <= 0 then
        return;
    fi
    await time.sleep(10ms);
    await bounded_recursive(remaining - 1);
}

pub async fnc consume_ticks(): void {
    val interval = time.interval(1s);
    // Cancellation/actor teardown must close the interval.
    while should_continue() do
        val tick = await interval.next();
        process_tick(tick);
    end
    interval.cancel();
}

pub suspends fnc throttled(): void {
    // A suspension point, NOT Thread.sleep / blocking pthread wait.
    time.sleep_blocking(25ms);
}
```

These examples describe required lowering, not a new execution thread for each
nested `await`. Recursive async calls use bounded owned continuation frames,
not an unbounded Java/C++ native call stack. Suspension across `rt borrow`,
`rt borrow mut`, actor-state critical sections and FFI resource loans must obey
the existing borrow and actor-lifetime checker. No implicit actor-state unlock
on `await`. The same timer future must work in `select` / timeout arms once
both backend select lowering paths are supported.

## Mandatory scheduler / ownership / cancellation invariants

1. Use a monotonic relative clock (Java executor / `nanoTime`; native steady/monotonic clock). Never compute elapsed waits from timezone or wall-clock jumps.
2. A clock/broker callback only **settles a Future or marks a token ready**. It cannot run guest code, borrow an actor heap, bypass an actor's execution lease, or directly resume an actor.
3. The producer may be on a different host thread; `await` always reschedules the continuation onto its original owning scheduler. Even already-completed timers resume on a later logical turn.
4. Close/context shutdown/actor teardown cancels registered timers and detaches pending continuations. Physical frame reclamation waits for any race-losing callbacks to quiesce.
5. Bound pending timer count, registrations, timeout notifications and interval backlog. Reject excess admissions with a typed resource error. Do not silently create a thread, actor, or queued closure per tick.
6. An interval's `next()` has **one outstanding consumer**. No timer runs until `next()` is requested; missed periods are not accumulated. This is **fixed delay**, not a fixed-rate catch-up stream. Callback intervals rearm only after the prior callback finishes.
7. Cancellation vs expiration vs shutdown has one linearization winner. A stale timer identifier, actor generation or scheduler domain must never wake a replacement continuation. Cancellation is not permission to reclaim a still-running guest frame.
8. A timer is a context/actor-owned capability, not an actor-sendable raw closure. Cross-actor subscription uses an explicit mailbox/message or channel; no implicit shared mutable state.
9. Guest callback errors follow actor supervision or task error propagation. They must not be silently swallowed on the clock thread.
10. Timing is best effort: deadlines specify **not-before** dispatch; no hard real-time wakeup guarantee. Clock and scheduler dispatch fairness can add delay.

## Actual implementation status

- **Java/GraalVM:** `OresTimers.afterNanos/sleepNanos` create cancellable `OresFuture<Void>` values, `Interval.next()` provides demand-driven ticks, and a bounded per-owner admission set is cancelled on close. Existing `OresScheduler` turns already support awaiting these futures. This is a runtime foundation, **not yet the exported `time.*` language module**.
- **LLVM:** `NativeTimerQueue` provides bounded monotonic deadline registration, deterministic due polling, cancellation, owner-domain stale-token fencing and teardown. Its poll result is a CONTROL-plane notification token, **not a source-level Future or continuation**. No guest lowering is implied.
- **Not complete:** source-level Duration literals, builtins/stdlib bindings, compiler effect checking for `sleep_blocking`, LLVM `Future<T>`/await continuation frames, `set_timeout` / `set_interval` guest callback admission, unified `select` timeout lowering, and compiler-generated lifecycle deregistration.

## Verification gates

Java: completion-on-scheduler, fresh dispatch for ready timers, cancellation
before/after expiry, owner shutdown, interval backpressure, timer admission
limits, and actor teardown/borrow scope. LLVM: deterministic virtual monotonic
timestamps, deadline ordering, cancel/poll race (exactly one winner), token
domain fencing, bounds/overflow, and close. Next add differential tests that
execute the **same .ores sources** on both runtimes, clock-controlled model
checks for cancellation and actor lifetime, and sanitizer-backed native tests.

Do **not** mark source API parity as complete on the strength of host-only
runtime tests; reject unsupported LLVM async source forms explicitly until the
native continuation lowerer is integrated.
