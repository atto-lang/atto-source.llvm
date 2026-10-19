# Native OresVM pthread carrier locality (draft)

## Implemented native host scheduling changes

This branch modifies the **actual** C11 `NativeActorScheduler` worker loop without allocating a thread per actor or modifying Oreslang actor ownership. It records the last pthread slot on each actor's scheduler-owned lane (under the scheduler mutex) and makes the existing ready scan three-stage:

1. Globally prefer authenticated supervisor CONTROL envelopes (unchanged and never delegated to DATA).
2. Prefer runnable DATA for actor lanes last executed by this worker.
3. Immediately fall back to any other runnable DATA when the preferred worker is unavailable.

The scheduler may migrate a runnable actor on any turn. There is **no blocking/grace interval to wait for affinity** and no actor pinned to a specific pthread. Exclusive actor execution claims from `NativeExecutionAuthority`, mailbox admissions, FIFO per actor, causal/control ordering, four scheduler domain ownership, lazy pthread creation and retirement remain unchanged.

The diagnostic host-only functions `ores_native_actor_scheduler_current_carrier_slot()`, `...preferred_carrier_hits()`, and `...carrier_migrations()` expose best-effort reuse. These are not a guest capability, a CPU ID, or a permission to mutate actor-owned data. `carrier_slot` returns -1 when not executing inside a native scheduler pthread. Overflow-safe saturating counters track reuse and migrations.

## Host-opt-in pthread binding on Linux

Set `ORES_NATIVE_CARRIER_CPU_AFFINITY=1` **in the trusted host environment before opening the scheduler** to request one eligible logical CPU for each worker. The worker samples its own Linux effective `sched_getaffinity()` mask and calls `pthread_setaffinity_np()` on itself with a single allowed CPU chosen modulo the admitted mask. If unsupported, denied, or beyond the fixed CPU_SETSIZE range, the request fails open into the ordinary OS scheduler, and `ores_native_actor_scheduler_current_cpu_target()` reports -1.

Default remains unpinned (`ORES_NATIVE_CARRIER_CPU_AFFINITY=0`). This binding is **not** an exclusive core allocation, a NUMA-first memory policy, PREEMPT_RT, `SCHED_FIFO`, `SCHED_DEADLINE` or real-time certification. OS cpuset changes/hotplug and topology mapping are not yet dynamically reprobed. It never expands an OS-allowed CPU set.

## Data-oriented scheduling boundary

Worker-indexed immutable metadata and per-actor hot scheduler slots improve the odds of reusing cached actor state, but **they do not make the actor's private heap worker-owned**. An actor's arena remains owned by that actor across carrier changes, Tasks remain logical continuations with scoped `Send/Sync` permissions, and the supervisor CONTROL domain must be prioritized even when an actor preferred worker is busy. Runtime SoA/AoSoA representation in the Java #1480 series is a separate data-layout concern, not a precondition for these semantics.

## Tests and limitations

`native_actor_carrier_locality_test.c` executes the real C actor scheduler, verifying 96 successive turns on a one-worker pthread (95 preferred-carrier hits), concurrent 8-actor/4-worker scheduling with per-actor FIFO/exclusivity, actor execution authority, cross-carrier diagnostic range, and correct stop/drain/destroy. Both default and host CPU-affinity-opt-in variants are registered as CTests with strict C11 warnings.

**Not yet proven:** source compiler Task handoffs, exact-head CMake/CTest, native ASan/UBSan, actual OS migration/cycle measurements, LLC/NUMA topology discovery, hard-core guarantees, and Java/LLVM source-level affinity syntax parity. Optimize only after source-exact CI and benchmark evidence against the unmodified shared ready scan.

Related: Java #301, Java #303, Java #1025, LLVM #15, Java triple-layout #1480, and `oreslang-common-source` typed Task ownership #90.

## Audit hardening — October 19, 2026

- The native scheduling loop now forces a globally fair DATA decision after at most eight successive preferred-worker DATA choices. CONTROL retains global priority at every decision.
- A regression queues 32 hot-actor messages against one new cold actor on a single worker and checks the cold actor is dispatched within the first nine hot callbacks.
- Opt-in Linux CPU affinity distributes the first carriers of independent scheduler domains by scheduler-domain ID; otherwise all domains could be pinned to the same first allowed CPU.
- Default affinity is still off, masks remain OS restricted, and failure falls back to ordinary Linux scheduling. No hard realtime, OS-thread ownership or NUMA-local memory guarantee is claimed.
- Native compiler, ASan/UBSan, and exact-head CTest remain mandatory merge gates.
