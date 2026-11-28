# OresVM / OresScheduler continuation cache-locality audit (2026-11-18)

This change is **native host-scheduler code**, not a guest API. The public AttoLang
semantics do not change; no new guest capability is exposed. It is compatible
with static AOT and actor-owned zero-guest-GC frames. Java parity remains a
separate verification task; do not claim it from this native-only PR.

## Execution boundaries

- The process-level VM CONTROL coordinator does not execute actor callbacks.
- CONTROL, PRIVATE, SHARED, and UNTRUSTED native scheduling domains own separate
  carrier pools and queues. CONTROL notices preempt DATA/TASK/WAKE within each
  native actor scheduler; locality does not override CONTROL FIFO.
- A continuation belongs to the owning actor/scheduler, *not* to a pthread.
  The physical actor execution claim still serializes access across migrations.
- Actor STOP, PANIC, cancellation and quiescence retain their existing fences.
  Hard CPU affinity, if configured, remains mandatory.

## Cache-locality hardening

- A scheduler with **one started carrier** now skips the bounded actor ready-
  queue affinity scan and takes FIFO head. With only one carrier, affinity
  cannot improve warm-cache placement; scanning unrelated actor nodes increases
  L1/L2 metadata touches. The same applies when the window is zero or one.
- Each actor lane already stores a last-carrier hint. Reuse that hint under the
  existing dispatch mutex to count actual carrier changes; no syscalls, new
  allocation, or additional atomic operations on the dispatch hot path.
- Count WAKE/TASK migrations separately from all-turn migrations. Both
  counters saturate and are informational, not scheduler admission authority.
- An actor's first turn does not count as migration; a newly spawned actor
  gets a cleared last-carrier hint.

## Verification and performance methodology

The source regression `native_actor_carrier_affinity_test.c` compares its
independently recorded pthread identities against the new migration counter,
checks DATA is excluded from the continuation metric, and covers the
single-carrier FIFO fast path. Existing native tests for scheduler priority,
continuation escrow, teardown and physical-turn gates must also pass.

On a Linux host, run the *same* binary/fixture with 1 vs 4+ carriers and
windows 0 vs 8. Keep affinity/cpuset, CPU quota, actor count, payload, load and
sampling duration fixed; report ops/s and p50/p95/p99 completion latency.
Use `perf stat -r 10 -e cycles,instructions,cache-references,cache-misses,L1-dcache-load-misses <binary>`
where supported. For L2 misses, choose a CPU-specific event from
`perf list` (for example Intel `L2_RQSTS.MISS` where available). Sample
scheduler lock contention, CPU migrations and per-domain carrier counts too.
These events may be unavailable or permission-gated; never replace them with
carrier migration counters while describing results as hardware-cache data.

**Execution evidence:** source inspection only in this PR construction. Full
CMake/CTest (Debug/Release), ASan, UBSan, native guest AOT and hardware perf
runs are **NOT EXECUTED** until stepful exact-head results are linked.
