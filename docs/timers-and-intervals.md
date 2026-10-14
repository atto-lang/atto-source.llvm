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

- **Java/GraalVM:** `OresTimers.afterNanos/sleepNanos` create cancellable `OresFuture<Void>` values. `Interval.next()` provides demand-driven ticks and a cancellable, single-consumer contract. Timer admissions are bounded per owner (4,096) and globally (16,384); live intervals are also bounded (4,096 per owner). Explicit owner `close()` revokes both pending timers and idle/pending interval capabilities. Existing `OresScheduler` turns can await the futures. **The OresVM/context and actor lifecycle do not automatically own/close this helper yet**. This is a runtime foundation, **not the exported `time.*` language module**.
- **LLVM:** `NativeTimerQueue` provides bounded monotonic deadline registration, deterministic due polling, cancellation and owner-domain fencing. A poll atomically moves a timer into an in-flight, still-capacity-charged delivery registry; a scheduler must exactly-once `claim_delivery` or `discard_delivery`, checking actor generation and executor authority before dispatch. Owner close revokes both scheduled and unclaimed-polled tokens. A successful claim is **not** a guest-continuation or lifetime lease. General typed Future/await lowering remains incomplete.
- **Not complete:** source-level Duration literals, builtins/stdlib bindings, compiler effect checking for `sleep_blocking`, LLVM `Future<T>`/await continuation frames, `set_timeout` / `set_interval` guest callback admission, unified `select` timeout lowering, and compiler-generated lifecycle deregistration.

## Hardening notes (October 14, 2026)

- **Cancellation linearization:** A pending timer can be cancelled. Once a native timer is polled, expiration has won over cancellation; the unclaimed delivery token remains charged to the owner until CONTROL dispatch claims it, a dropped continuation discards it, or owner close revokes it.
- **No guest reentry:** A native poll/claim is not authorization to call a guest continuation directly. The scheduler must still validate actor generation, owning domain, task admission and execution lease at actual dispatch.
- **Global load:** The Java runtime cap applies across all `OresTimers` instances sharing the executor, preventing bypass through many timer-owner instances. It is not a substitute for tenant-specific time quota and explicit OresVM-owned lifecycle wiring.
- **Revocation is not quiescence:** Calling `OresTimers.close()` or `NativeTimerQueue.close()` revokes admissions and outstanding unclaimed timer deliveries. It does **not** prove that a callback already running, a scheduler turn, or an actor-owned continuation frame has physically quiesced. Actor teardown must wait for the corresponding scheduler/producer exit acknowledgement before reclaiming heaps.
- **Evidence scope:** An isolated native C++17 test harness passed with GCC, Clang, and Clang ASan/UBSan. An isolated Java 21 harness also passed 500 interval-next vs owner-close races; it used a minimal stand-in `OresFuture`, so it does **not** validate the production OresFuture or the GraalVM interpreter. Full repository CI could not be validated: GitHub Actions reported failure with no steps/logs available. Source-level timer syntax is still an unimplemented proposal.

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
