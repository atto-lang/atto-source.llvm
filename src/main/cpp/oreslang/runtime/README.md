# Future LLVM native runtime

Track execution-domain/actor ABI work in [issue #1](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/issues/1).
This directory intentionally contains no executable actor or memory allocator.

Java reference classes: `ActorRuntime`, `OresScheduler`, `OresFuture`, `RuntimeGarbageCollector`.
Native phases: structured cancellation, generational wake IDs, pinned/relocatable async frames,
actor mailbox lease, supervisor trees, independent physical heap regions and bounded arenas.

No RT scheduler policy, zero-GC or actor isolation is currently implemented.

## Dedicated hungry actor idle/I/O semantics — design only

Read [Hungry isolated actor idle/I/O contract](../../../../../docs/HUNGRY_ACTOR_IDLE_IO.md).
A dedicated thread is **not** a permanently busy-spinning thread: an unresolved
`await` or blocking `select` suspends an actor continuation and, if no eligible
work remains, the carrier ordinarily parks after a bounded polling window.
A long (100–200 ms) I/O operation belongs on an async I/O path/broker; the
actor retains its thread/core reservation, and a generation-fenced completion
wakes it. Strict no-GC hungry actors do not run arbitrary GC while idle.
No actor, I/O, affinity, reservation, or GC behavior described by the contract
exists yet in this C++ runtime scaffold.

## Audit implementation slice: Linux ParkingSignal (experimental)

`ParkingSignal.hpp/.cpp` supplies **one carrier-bound waiter with concurrent
notifiers**, backed by nonblocking `eventfd` and a monotonic `poll(2)` deadline.
It prevents lost notifications across the ready-check/park race, verifies the
dedicated carrier thread identity, handles spurious notifications, and makes
cancellation wake the carrier when the caller publishes stop + notifies.

The API is **NOT** an actor, mailbox, ownership checker, continuation scheduler,
CPU isolation manager, async I/O broker or zero-GC allocator. It does not claim
kernel-level lock freedom; `eventfd` uses syscalls and internal kernel locking.
It only guarantees that no *user-space actor heap mutex* is required by this
notification primitive. A bounded lock-free data queue and separate reserved
control/completion capacity are future work.

The producer MUST publish a thread-safe ready predicate/queued completion
before `notify()` (release/acquire); notification alone carries no payload.
The caller MUST join its single waiter and quiesce **all** concurrent notifiers
before destroying the signal. Error paths throw, so this prototype is **not**
certified for no-allocation/hard realtime hot paths. Install/warm it before
entering realtime scheduling; validate scheduling and memory resources
separately.

Linux CTest target: `parking_signal_wake_cancel_races`.
Implementation scope and remaining hazards: [audit](../../../../../docs/HUNGRY_ACTOR_AUDIT.md).
