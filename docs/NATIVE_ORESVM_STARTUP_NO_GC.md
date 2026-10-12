# LLVM OresVM cold-start and zero tracing-GC contract

The native backend does **not** instantiate Java's `RuntimeGarbageCollector`,
a collector thread, a GC root registry, or a periodic GC sweep timer. Native
arena ownership is explicit. This is not a claim that the host OS allocator
never allocates or pauses, and the current source compiler has incomplete
class/async/actor ownership lowering.

## Runtime lifecycle

| Phase | Eager host-owned state | Threads/allocations deferred |
| --- | --- | --- |
| CLI parse and native compilation | Build flags, module graph, verified code | No actor scheduler or GC service |
| Native actor scheduler `open()` | Bounded mail rings, actor slots, capability domain, synchronization, maximum worker count | **Zero pthread carrier threads** |
| First actor slot `spawn()` | Actor incarnation/identity; start exactly **one pool scheduler CONTROL thread** | Still **zero carrier workers** |
| First **accepted** user/CONTROL message | Scheduler signals a blocking condition and issues bounded worker-dispatch tickets | Start one carrier, grow toward the configured maximum according to outstanding demand |
| Actor turns | Actor-owned arena + turn epochs, scratch scopes, pinned continuations/loans | No tracing GC |
| Stop and retirement | Fence new admission, drain accepted work, join only created workers | Deterministic zeroization/free after loan and callback quiescence |

`ores_native_actor_scheduler_started_workers` is a host diagnostic. It must
read zero after `open()` and `spawn()` and one after the first accepted item, and never exceed the configured maximum while growing with queued demand. A rejected invalid/stale actor reference never
starts workers. A worker-start failure is fail-closed: no runnable work is
published, any partially started workers are joined during retirement.

The scheduler is a multiplexor, **not** one thread per actor or group.
ActorGroup membership/communications do not themselves require new carriers.
One dedicated scheduler control thread per activated native pool selects ready actor turns, prioritizes CONTROL tickets, and waits on a condition variable; workers only execute scheduler-issued tickets. There is no busy polling, no actor code on the scheduler thread, and no extra scheduler thread per actor. The separate OresVM master-to-pool event plumbing is not yet part of this native host ABI.\n\nA serial actor's single logical handler lease survives await/cooperate even
when it is not physically running; no new message may overtake that handler.

## Memory

* Function-local owners use borrow-checked lexical drops and stack/scratch
  regions; persistent actor objects use explicitly owned native arenas.
* A suspended async frame, FFI lease or outstanding borrow prevents physical
  destruction. Supervisor retirement first fences new external callbacks,
  timers and resumption tickets, then waits for quiescence.
* Graph edges are non-owning generational handles so graph cycles do not
  introduce reference-counted reclamation cycles.
* No finalizer thread may asynchronously free an arena in use by a carrier.
* No bridge to JVM `RuntimeGarbageCollector` is allowed in native source code.

## Scope / remaining gates

This change implements **lazy host scheduler worker activation**, not the
complete LLVM CLI process launcher/OresVM root scheduling ABI. Existing
`oreslang-llvmc` is a compiler that emits LLVM IR. Native guest actor
`spawn/await/cooperate` parity, class/struct arena lowering, deterministic
`defer` on all async cancellation paths, and zero-GC whole-program audit are
distinct release blockers. Do not claim that supported native host tests
establish all those compiler/runtime semantics.
