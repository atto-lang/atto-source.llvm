# Timer engine contract v1

Shared design for the runtime's logical timer engine. Each `atto-source.*`
backend implements it in its own code; behaviour is checked by a shared
conformance vector. Part of the main control loop's TIMERS phase (design brief:
`atto-lang/control-loop-design/CHATGPT-DESIGN-SUMMARY.md` in the workspace).

| Backend | Wheel | Driver | Tests |
|---|---|---|---|
| atto-source.java | `runtime/OresTimerWheel.java` | `runtime/MonotonicTimerDriver.java` (used by `OresTimers`) | `OresTimerWheelTest` |
| atto-source.llvm | `runtime/NativeTimerWheel.{h,c}` | `runtime/NativeTimerDriver.{h,c}` | `tests/native_timer_wheel_test.c` (CTest `native_timer_wheel_engine_and_conformance_vector`) |

## Why

The design requires that the runtime own its timers: a million logical timers
must not mean a million host timer objects, and only the nearest deadline is
armed with the host (`epoll_wait`/`parkNanos` timeout). Before v1, Java armed one
`ScheduledThreadPoolExecutor` task per timer (global cap 16384) and the native
`NativeTimerQueue` used `std::map`/`std::set` nodes behind a mutex, allocating
per arm and per poll, and was not wired to any wakeup.

## Engine (single owner, not thread-safe)

- **Structure.** Hierarchical timing wheel, 6 levels × 64 slots (2^36 ticks),
  plus an overflow list for later deadlines. A timer with expiry tick `e` and
  current tick `c` sits at level `floor(msb(e XOR c) / 6)`, slot
  `(e >> 6·level) & 63`. Its slot is therefore always ahead of the current
  position, and the first non-empty level holds the next event.
- **Time.** The caller passes monotonic nanoseconds (≥ 0) to every call, so a
  virtual clock can drive it deterministically. A reading earlier than the last
  one is clamped (monotonic). Tick size is configurable (drivers use 1 ms); tick
  size never changes firing precision.
- **Firing.** A timer fires on the first poll whose time is ≥ its deadline,
  never earlier, including sub-tick deadlines.
- **Order.** Emission is in expiry-tick order; within one tick, timers fire in
  the order they reached that tick's list (arm order unless a cascade
  intervened). Every list is FIFO.
- **Bounded draining.** `poll(now, max)` emits at most `max` timers. The rest stay
  queued (DUE) and remain cancellable until emitted.
- **Handles.** `(generation << 32) | (index + 1)`; 0 is invalid. Freeing a record
  advances its generation, so a stale handle can't cancel or observe a reused
  record. Cancel is O(1) and returns true only for a live timer.
- **Next deadline.** `next_deadline()` returns the current time if timers are
  due, otherwise a lower bound on the next deadline (a cascade boundary may come
  first), or "none". It is never later than the true next deadline, so a parked
  driver is never late.
- **Idle gaps.** Advancing jumps between occupied slots using per-level
  occupancy bitmaps. A month-long idle gap costs at most a few cascades, not one
  step per tick.
- **Memory.** Records live in parallel arrays indexed by a 32-bit slot number.
  Native: one slab allocated at init. Java: arrays that grow up to a cap, then
  stop. Arm, cancel and poll do not allocate after warm-up.
- **Safety.** The engine never runs guest code. An expired record carries only
  a payload (Java: an attachment) that the owner turns into a scheduler wake.

## Driver

One clock thread per wheel parks until `next_deadline()`. Arming a timer
earlier than the deadline the driver is parked for wakes it, race-free: the
parked-until value is published under the wheel lock before parking (Java:
`LockSupport` permit; native: condition variable on `CLOCK_MONOTONIC`, or the
relative timed wait on Darwin). Expired actions run on the driver thread,
outside the lock, at most 256 per wheel pass. They may only settle runtime
state. A driver is one TIMERS source; integrating timer expiry directly into
scheduler carriers' park path (`wait_until(next_deadline)`) is a v2 step.

## Conformance vector

Both test suites run the same seeded sequence: LCG
`x = x·6364136223846793005 + 1442695040888963407`, values from `x >> 33`, 20,000
steps of arm/cancel/poll over a 1 ms-tick wheel of capacity 4096, with delays
spanning sub-tick to about 2^42 ns and occasional clock jumps of up to 10^12 ns,
then a final drain. Each emitted payload folds into
`checksum = checksum·1000003 + (id XOR (pollIndex << 20))` (mod 2^64). The
required result is:

```
fired=11085 cancelled=793 full=0 polls=4074 checksum=bcb6942ca7275487
```

A backend whose wheel diverges in firing time, order, bounded draining or
cancellation produces a different checksum.

## Verified (2026-11-28)

- Java: `OresTimerWheelTest` 12/12 (includes 200k-op differential vs a sorted
  model, 1M timers, steady-state zero-allocation check); `OresTimersTest` 35/35
  and `ExecutionAffineSleepRuntimeTest` 7/7 on the new driver.
- Native: the test passes on macOS arm64 (plain, ASan+UBSan, TSan) and Linux
  aarch64 Ubuntu 24.04 (plain, ASan+UBSan, TSan with ASLR disabled), and the
  vector matches.
