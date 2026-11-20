# Native actor scheduler latency

Status: **host-side C scheduler change, not source-language lowering.**
None of this is a hard-RT certificate. See [HARD_REALTIME.md](HARD_REALTIME.md).

Goal: the scheduler should add less latency than the kernel/hardware it
runs on. `tests/native_actor_scheduler_latency_bench.c` measures that
directly, by printing two floors next to the scheduler numbers:

- `floor_condvar`: two pthreads ping-pong via mutex + condvar. Each hop is
  one kernel sleep + wake.
- `floor_spin`: two pthreads ping-pong via one atomic. This is the
  cache-line transfer cost, with no kernel involved.

```sh
build/oreslang-native-actor-scheduler-latency-bench                 # defaults
build/oreslang-native-actor-scheduler-latency-bench --lock throughput \
    --spin-ns 20000 --cpus 2,3,4,5                                  # tuned (Linux)
```

## Audit findings (before)

1. `find_ready` scanned every actor lane up to 3 times per dispatch, with a
   `%` per step, while holding the one scheduler lock. With 1024 actor
   slots that is ~3000 lane reads per turn, for every carrier.
2. A finished turn took the lock 2–3 times, then took it again to look for
   the next turn.
3. Every send, CONTROL and wake publish called `pthread_cond_signal`, even
   when the target lane was already queued or running, or no carrier was
   parked.
4. Idle carriers parked in the kernel immediately; there was no bounded
   spin, so every cross-carrier handoff paid a futex sleep + wake.
5. No CPU pinning. Mailbox rings used `%` (a divide) on every message.
6. The scheduler mutex is always `PTHREAD_PRIO_INHERIT`. On Linux every
   *contended* PI acquire goes through the kernel (`FUTEX_LOCK_PI`, no
   user-space spinning). That costs ~25x in throughput under contention.

## Changes

- **O(1) ready queues.** Intrusive FIFO per turn class (CONTROL, WAKE,
  DATA). A lane is linked into class k exactly when it is idle and has
  class-k work; `rq_sync_locked` re-derives that after every state change.
  Priority order and the two-turn warm-cache rule are unchanged.
  `ORESLANG_NATIVE_SCHED_VALIDATE` cross-checks every queue against an O(N)
  scan after each mutation (enabled in the stress tests).
- **One lock round trip per turn.** Without a turn gate, completion and the
  next dispatch share one critical section. With a gate, the existing
  unlock/release/relock order is preserved.
- **Wake only for new runnable work.** A producer signals only when a lane
  newly becomes runnable, only if a carrier is parked, and not if a carrier
  is spinning. After a dispatch, if more lanes are still runnable, the
  carrier signals one more ("chain wake"), so parallelism is preserved.
- **Bounded idle spin** (`configure_idle_spin`, off by default, max 1 ms).
  A spinning carrier polls an atomic work epoch, which every admission and
  every lifecycle broadcast (retire, gate, signal_control) bumps.
  Spin-vs-park decisions are made under the lock, so skipping a signal
  cannot lose a wakeup.
- **CPU affinity** (`configure_cpu_affinity`, Linux only, fails closed
  elsewhere). Set in the pthread attributes, so a carrier never runs
  outside its CPU. `cpu_affinity_observed` reports what each carrier read
  back from the OS, not what was requested. `prestart_carriers` now waits
  until every carrier has checked in.
- **Lock protocol choice at open** (`open_with_options`). The default stays
  `PRIORITY_INHERIT`; every existing constructor is unchanged. `THROUGHPUT`
  uses a plain (glibc: adaptive) mutex and makes
  `priority_inheritance_enabled()` report 0, so hard-RT admission that
  requires PI fails closed.

## Results

Means/p50 in ns; one benchmark run each, so treat small differences as noise.

macOS 26, Apple M4 Max (kernel floor mean ≈ 2,000; spin floor p50 = 42):

| scenario | before | after | after + 20 µs spin |
|---|---:|---:|---:|
| ping-pong 1024 actors / 2 workers, mean | 2,091 | 92 | 90 |
| ping-pong 1024 actors / 8 workers, mean | 10,228 | 1,884 | 192 |
| ping-pong 1024 actors / 8 workers, p99 | 35,791 | 9,708 | 3,584 |
| fan-in 1024 actors / 4 active, ns/turn | 6,145 | 174 | 180 |
| fan-in 16 actors / 16 active, ns/turn | 686 | 322 | 307 |

Linux 6.10 (Docker Desktop VM, aarch64, 12 vCPU, GCC 14; kernel floor mean
≈ 11,000, spin floor p50 = 84):

| scenario | before | after (PI lock) | after (throughput lock) | + spin + pinned |
|---|---:|---:|---:|---:|
| ping-pong 1024 / 2 workers, mean | 12,964 | 180 | 216 | 168 |
| ping-pong 1024 / 8 workers, mean | 98,595 | 11,261 | 3,745 | 611 |
| fan-in 16 / 16, ns/turn | 135,936 | 27,099 | 336 | 323 |
| fan-in 1024 / 64 active / 8 workers, ns/turn | 69,501 | 49,864 | 697 | 582 |

In every ping-pong scenario the scheduler now costs no more than one kernel
wake (parked carriers), or close to the cache-line floor (spinning).
Under contention with the default PI lock, the remaining cost is the
kernel's PI futex path.

## Not done / next

- One scheduler lock still serializes producers and carriers. The next step
  for hard RT is the dedicated per-core RT domain in HARD_REALTIME.md:
  statically assigned actors, per-core SPSC rings, no shared lock.
- Wake affinity (letting the sending carrier run the target first rather
  than waking another carrier) would remove the remaining kernel wake in
  the 8-worker ping-pong without spin. It can delay the target behind a
  long handler, so it is left out.
- Measured on a VM and a laptop, not an isolated PREEMPT_RT host. Use
  `rtla timerlat`/`osnoise` and `cyclictest` on the real target.
