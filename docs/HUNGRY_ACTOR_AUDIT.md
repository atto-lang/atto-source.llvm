# Hungry actor idle, I/O, and realtime safety audit

**Scope:** Native C++/LLVM hungry isolated actor proposal and one Linux notification/parking primitive. **Status:** partial proof-of-concept; not a production actor scheduler. This audit does not certify GC freedom, hard realtime, CPU exclusivity, lock-free mailboxes or async actor continuations.

## Audit findings and dispositions

| Severity | Finding | Hardening / acceptance requirement |
| --- | --- | --- |
| Critical | Notification can race with deciding to sleep | Linux nonblocking `eventfd` retains readiness until read; caller checks published ready/stop predicates before and after poll. Tested before-wait and competing notifications. A completion queue is **still required**. |
| Critical | A wake is not a completion payload | Notification is a **hint** only. Producer publishes an owned result (release) before signaling, consumer reads it (acquire). Spurious notifications cannot run user code. Tests cover false wake. |
| Critical | Ordinary bounded mailbox full can prevent stop/completion | Control, cancellation, and continuation-completion traffic need **reserved capacity / separate queues**; a full user mailbox must not prevent supervisor stop. Not implemented; design gate. |
| Critical | Cancellation vs I/O completion can double-resume/free in-flight buffers | Operation-specific generation/epoch and single terminal CAS state, kernel/worker buffer ownership until acknowledged, cancellation wake and late-completion drop. Not implemented; future race test. |
| High | Two waiters can consume one another's eventfd notifications | `ParkingSignal` rejects concurrent waiting and a later call from a different `std::thread::id`. The supervisor must own thread lifetime; thread ID alone is not a secure never-reused generation token. |
| High | Reusing actor thread while suspended breaks isolation/serial semantics | Strict dedicated carrier never executes another actor; suspended handler blocks normal actor turns by default. Runtime still needs single-execution lease and self-await/cycle deadlock detection. |
| High | Confusing CPU pinning with physical-core exclusivity | Affinity is **not** isolation or a guarantee that CPU cache is untouched. Check effective cpuset/SMT siblings, active isolation partition, IRQ and OS housekeeping, hotplug, cgroup changes and RT policy. |
| High | `SCHED_FIFO` busy spinning can starve stop/control | Use bounded spin-then-park by default; control checks within spin and compute checkpoints; RT watchdog/emergency out-of-band supervisor. Untrusted actors cannot request exclusive realtime privileges. |
| High | GC during an arbitrary idle gap races with completion | Strict profile never performs implicit tracing/compaction on parked carrier; paused continuation retains roots. Actual allocator, cycle reclamation and ownership proofs remain unimplemented. |
| High | Blocking syscall / FFI can steal realtime native thread | No unbounded blocking FFI in strict profile. Use async I/O broker and cancellation-safe owned/pinned buffers; some `io_uring` operations can still use helpers/block. |
| Medium | Thread parking while queue is full or notifier fails | Notification failure must be a visible infrastructure failure; no silent swallowed `EBADF`/`EIO`. `eventfd` `EAGAIN` is safe only because its counter already indicates readability. |
| Medium | Realtime path can allocate/throw despite no-GC intent | Current prototype constructs eventfd on startup but **throws exceptions on error**. Not realtime-certified; replace throwing hot paths with explicitly accounted statuses, preallocation and fail-closed supervisor before integration. |
| Medium | 32-bit futex wake sequences can wrap in high-throughput long-lived actors | This prototype uses 64-bit eventfd counter/readiness instead. Eventfd still isn't a message log and must never serve as an operation ID. Future futex design must prove rollover handling. |
| Medium | Wall-time timeout can be inaccurate | Wait timeout uses `steady_clock`, rounds sub-ms waits upward with `poll`, rechecks predicates on EINTR. No microsecond precision or hard bound is claimed. |
| Medium | Destroying signal while any producer/waiter is active leads to FD reuse and races | Only destroy after quiescing publishers and joining the owner thread. Explicitly documented lifetime contract; future managed handle needs a shutdown barrier and ref-counted ownership. |

## Current executable scope

`src/main/cpp/oreslang/runtime/ParkingSignal.{hpp,cpp}` is one Linux eventfd-based **notification** primitive, with:
- A single dedicated waiter claim guarded by an atomic flag; concurrent notifiers permitted.
- Nonblocking eventfd writes and poll sleeps; failed kernel syscalls fail explicitly.
- Release/acquire predicate synchronization required of the caller, not silently inferred from notification.
- Monotonic timeout, stop-first arbitration, EINTR/spurious-wake rechecks.
- Test cases for ready-before-wait, signal-before-park, a 150 ms simulated I/O completion, spurious notify, cancellation wake, thread migration rejection, timeout, and 4-producer notifications across repeated runs.
- CTest integration only on Linux; this does **not** modify the LLVM compiler or the actual application actor runtime (not implemented).

## Core concurrency invariants to prove next

1. **Linearized completion:** each (actor generation, operation generation) reaches exactly one terminal state: completed, timed out, canceled or faulted. Competing paths cannot resume it twice.
2. **No completion drops:** bounded queues must reserve control/completion capacity and expose backpressure. No silent overwrites or dropped wakeups when mailbox is full.
3. **No use-after-cancel:** outstanding I/O buffer remains owned/pinned until asynchronous kernel/user callback releases it; actor cancellation alone does not free it.
4. **No cross-thread heap access:** I/O workers and notifier threads cannot read or mutate actor-local Oreslang objects; transfer only typed owned immutable/copy buffers.
5. **No accidental reentrancy:** default serial handler continues before processing next normal turn. Actor self-request awaiting its own queued response must fail/cancel or invoke a separately approved reentrancy mechanism.
6. **Stop always deliverable:** watchdog, supervisor stop and cancellation must have an out-of-band unblocked path independent of user mailbox pressure and actor code cooperation.
7. **Hot path accounting:** no heap growth, tracing GC, loader/JIT, blocking FFI or unbounded finalizer on the strict carrier. Deterministic drops can still have latency and require budgets.
8. **No premature realtime claims:** warm-up, prefaulting, CPU/SMT topology, actual cpuset state and effective scheduler policy must be *observed* at runtime; a successfully called `pthread_setaffinity_np` alone is not sufficient.

## Additional risk considerations

- **Core occupancy:** when parked the core may enter idle power states or be reused by kernel/firmware interrupts. Exclusive allocation means administrative admission, not a cryptographic CPU lock. If a fully reserved sleeping CPU is desired, the host must keep other runnable processes away; do not assume `SCHED_FIFO` alone accomplishes that.
- **Latency tradeoff:** a `spin_then_park` threshold (10 µs in earlier design) is a benchmark hypothesis. The current primitive implements **park only**, not spin/affinity/RT admission.
- **Kernel and tooling:** Linux eventfd uses internal kernel serialization; the primitive is not a blanket lock-free runtime guarantee. Each wake incurs syscalls; benchmark busy-poll and futex alternatives later, accounting for lost wake/rollover proofs.
- **GC claims:** `gc=disabled` in a TOML sketch is not enforcement. Verify guest/native allocations, deferred drop latency, cyclic reference strategy, paused stack roots, and realtime page faults separately.
- **Cgroup and OS dynamics:** online CPU set, partition validity, IRQ affinity, RT throttling, SMT and NUMA can change over time; the supervisor must detect admission loss and either fail safely or downgrade only with explicit permission.

## Verification and non-claims

Local development may compile `ParkingSignal` with `-fsanitize=address,undefined`; GitHub CTest still must execute at an immutable Git tree SHA. A no-runner-allocated Actions failure is not test evidence.

**Does not exist yet:** actor scheduler integration, private heaps, native borrow checker, hot-path allocation bounds, a bounded lock-free mailbox, completion generations, spin-then-park policy, I/O broker, CPU exclusivity admission, realtime watchdog, native trusted actor AOT, or real GC strategy.

Detailed proposed semantics: [HUNGRY_ACTOR_IDLE_IO.md](HUNGRY_ACTOR_IDLE_IO.md).
Linux references: [eventfd(2)](https://man7.org/linux/man-pages/man2/eventfd.2.html), [poll(2)](https://man7.org/linux/man-pages/man2/poll.2.html), [sched(7)](https://man7.org/linux/man-pages/man7/sched.7.html), and [kernel CPU isolation](https://www.kernel.org/doc/html/latest/admin-guide/cpu-isolation.html).
