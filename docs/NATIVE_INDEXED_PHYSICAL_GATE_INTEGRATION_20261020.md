# Mainline integration: hierarchical actor ready indexes + shared physical turn permits

Status: 2026-10-20, draft integration. Native trusted-host runtime only. No
LLVM-generated actor-source execution, guest NUMA API, GPU placement, or
measured cache-speedup claim follows from this change.

## Why another integration branch?

Existing stacked native PRs #672 -> #676 -> #677 implement same-pthread
reuse, 1024-lane indexed ready bitmaps, and 65536-lane dynamic capacity.
Their ancestor is older than mainline's process-wide physical-turn gate.
Blindly replacing mainline NativeActorScheduler.c with the older stack
would discard global nonparking admission and its gate-wake / lifetime
barriers. The integration branch starts from current main and ports
the ready occupancy concepts without replacing the physical-turn code.

This is an **alternative semantic integration**, NOT a reason to merge both
the old stack and this branch independently. Choose one tested lineage,
review old changes, then retarget or close redundant drafts explicitly.

## Executed runtime behavior (source implementation, not certification)

- Independently allocated CONTROL and DATA leaf indexes, 1 bit/actor,
  organized in 64-actor words. A second level has 1 bit per nonempty
  64-actor leaf word. At most 1024 leaf words and 16 summary words for
  65536 actor slots. All updates occur while holding the scheduler mutex.
- Admission, dispatch and physical completion maintain bitmaps. An active
  actor has no ready bits even if DATA/CONTROL is queued concurrently.
  Terminal stop does not silently discard previously admitted mail.
- Global CONTROL remains ahead of DATA and ahead of cache-local
  same-carrier reuse. Per-actor FIFO remains unchanged. Warm DATA still
  gets no more than two consecutive same-carrier turns; other workers
  may execute nonactive actors but cannot concurrently enter the same
  actor authority.
- A selection does **not** advance the round-robin cursor until after
  the physical turn permit is actually acquired. The old scheduler
  mutated next_actor even if the process-wide permit was denied; repeated
  gate failures could perturb scheduling fairness without executing work.
- No actor may enter its callback without its physical execution claim.
  We preserve the gate's registered-waiter recheck, cross-domain signal,
  gate release while outside scheduler mutex, and final lifecycle barrier.
- Above 1024 actors, at most 1048576 mailbox cells (8 MiB) are admitted;
  <=1024 retain previous admission bounds. Actor slots max at 65536.
  Both leaf arrays and both summary arrays are allocated at bootstrap
  and freed on constructor failure and quiescent destruction.

## Tests added to the real CMake manifest

- `native_actor_indexed_control_data_1024`: 1024 actor lanes, word
  boundaries, CONTROL/data concurrency, per-actor exclusivity and FIFO.
- `native_actor_dynamic_ready_65536`: 1025/4097/16385/65536 capacity,
  hierarchy boundaries, old/new mailbox budget admission, CONTROL/FIFO,
  cleanup and stale identity rejection.
- `native_actor_indexed_physical_turn_gate`: one external physical
  permit held while DATA and CONTROL queue; callback count stays zero,
  release wakes service, CONTROL goes first, each DATA actor runs once,
  and retirement frees the permit.

The existing `native_vm_cross_domain_physical_turn_limit`,
`native_actor_carrier_locality_control_priority`, generation/stop,
lazy-carrier failure, actor shard and supervisor tests are mandatory.

## Pending verification and performance gate

1. Exact PR head: CMake configure/build and CTest with strict GCC/Clang,
   real populated job steps, and ASan/UBSan. No source-repo 'failed'
   workflow with empty steps may be presented as compiler evidence.
2. Actor scheduling failure injection and release races, including
   scheduler retirement while a gate-waiting worker has outstanding mail.
3. Benchmark small/large idle actor populations for mutex hold time,
   CONTROL wake tail latency, migrations and L1D/L2/LLC miss counts;
   use the independent cache-perf harness in draft PR #754.
4. Compare performance against current main and stacked #677 from
   **identical workloads**. Do not claim that bitmaps alone establish CPU
   pinning, NUMA allocation or L1/L2 cache residency.

## Ownership/actor safety

An actor's single guest `state` schema remains wholly actor-owned, and
access is still confined to verified critical state loans. Scheduler
bitmap indexes contain no guest field pointers, actor data, device
handles or borrowed references. Task region disjointness/NUMA locality
and GPU device/queue affinity remain separate, feature-gated work.
