# Native per-core RT domain

Status: **host-side C dispatch substrate** (`NativeRtDomain.{h,c}`), the
"dedicated RT scheduler domain" from [HARD_REALTIME.md](HARD_REALTIME.md).
Not source-language lowering, and not a schedulability or WCET proof.

The general `NativeActorScheduler` serializes every send and every turn on one
mutex (see [NATIVE_SCHEDULER_LATENCY.md](NATIVE_SCHEDULER_LATENCY.md)). That
is the right trade for the general domain (any carrier can run any actor,
rich lifecycle), but it means tail latency depends on lock contention. The RT
domain has no scheduler lock at all.

## Design

- **One carrier per core.** Created at `start`, optionally pinned (Linux,
  pinned in the thread attributes) and `SCHED_FIFO`. If any carrier cannot be
  created with the requested policy, `start` stops the ones it made and
  returns 0. It never runs on fewer cores or a weaker policy.
  `affinity_observed` reports what each carrier read back from the OS.
- **Static placement.** Every actor is assigned to one core at open. An actor
  only ever runs on that core's single carrier, so per-actor exclusivity holds
  by construction, with no claim, lock or CAS per turn.
- **SPSC ring matrix.** One fixed-capacity ring per (sender, target core) and
  lane. The senders are the cores themselves (handlers send through the
  `self` sender they are given) plus a fixed number of external senders. Each
  sender has exactly one owning thread, enforced: use from another thread
  returns `INVALID`. Push and pop are one release store each, with cached
  opposite indexes, so an uncontended operation touches only its own line.
- **CONTROL before DATA.** A separate lane. The carrier drains CONTROL first
  and re-checks a per-core CONTROL hint between DATA turns, so queued
  supervisor traffic is not stuck behind a backlog.
- **Bounded memory.** Everything is allocated and touched at `open`.
  `send` and dispatch never allocate, lock or make a syscall, except to wake
  a parked core. A full ring returns `FULL` immediately; handlers must not
  spin on it.
- **Idle.** Spin for at most `idle_spin_ns` scanning the core's rings, then
  park. Parking is a Dekker handshake: the carrier publishes `sleeping` and
  re-checks the rings; a sender publishes its item and then checks
  `sleeping`. With seq_cst fences on both sides, a wakeup cannot be lost.
- **Quiescent stop.** Senders count a message *before* publishing it, and a
  core counts a turn *after* its handler returns. So total processed never
  exceeds total accepted, and equality means nothing is queued and no handler
  is running (a running handler's own message is accepted but not
  processed). `stop` waits for equality, then closes. Mail that handlers send
  while draining is delivered. External senders must be stopped first.

## Hardening

- **Sender ownership uses never-reused thread ids**, not TLS addresses
  (a later thread can be handed a dead thread's TLS address). Handing an
  external sender to another thread is explicit: `ores_rt_sender_release` is
  a release, and the next owner's claim is an acquire, so the old owner's last
  pushes happen-before the new owner's first.
- **Duplicate CPUs are rejected**: two carriers on one CPU, under SCHED_FIFO
  with idle spin, can starve each other.
- **The park mutex uses priority inheritance** where available. It is the
  only lock a carrier takes, and a low-priority sender holds it to wake one.
- **Bounded stop**: `ores_rt_domain_stop_timed` returns 0 if a handler is
  stuck, leaves the domain running, and can be retried.
- **No silent loss**: if an external sender keeps sending during `stop` (a
  contract violation), anything it slipped in is reported by
  `ores_rt_domain_undelivered`, so accepted == ran + undelivered always holds.
- **Turn budget**: `turn_budget_ns` counts turns that overran it per core
  (`budget_overruns`). Detection only; nothing is preempted.
- **Size overflow** of the ring allocation is rejected at open.

## Tests

`tests/native_rt_domain_test.c`, plus a TSan target:

- contract: bad options rejected, pinning fails closed off Linux, observed
  affinity on Linux, single-owner senders, `CLOSED` before start and after
  stop;
- CONTROL queued behind DATA runs first;
- drain: a handler that sends, waits for the receiver to run, then sends
  again must not be cut off by `stop` (200 rounds);
- timed stop on a stuck handler (times out, keeps running, then stops) and
  budget overrun counting;
- sender handoff: refused while owned, ordered after release;
- stop under a flooding external sender: accepted == ran + undelivered
  (50 rounds; this did catch messages that would otherwise be lost);
- lost-wakeup probe: 50,000 one-at-a-time handoffs racing the park path, each
  must complete within 1 s;
- stress: 3 producers × 40,000 messages to 64 actors on 4 cores, handlers
  forwarding cross-core, CONTROL mixed in. Checks exactly-once delivery,
  per-sender FIFO, placement and exclusivity, spin on and off.

Each failure mode was checked by breaking the code on purpose: dropping the
pre-park re-check (lost wakeup), dropping CONTROL priority, or counting
`accepted` after publishing (false quiescence) each fails the test 5/5.

## Measurements

`oreslang-native-actor-scheduler-latency-bench` prints `rt_*` scenarios next
to the kernel/spin floors and the general scheduler (`sched_*`). Means/p50 in
ns, one run each.

| | general scheduler | RT domain |
|---|---:|---:|
| macOS M4: same-core hop, mean | 86–99 | 14 |
| macOS M4: cross-core hop, 20 µs spin, p50 / p99 | 42 / 1,125–4,500 | 125 / 334 |
| macOS M4: fan-in 4 producers, ns/turn | 176–498 | 37–54 |
| Linux VM, pinned: cross-core hop, p50 / p99 | 83 / 2,167–6,750 | 125 / 500 |
| Linux VM, pinned: fan-in, ns/turn | 265–586 | 37 |

Without spin, a cross-core hop costs one kernel wake (it matches the condvar
floor). That is the remaining kernel cost, and the reason `idle_spin_ns`
exists for latency-critical cores.

## Not done

- Fixed-priority / EDF admission, per-actor period/deadline/WCET budgets and
  the `RT00x` diagnostics are not wired to this domain yet.
- No source-language lowering: compiled `.ores` actors do not target it yet.
- No per-actor lifecycle (spawn/stop/supervision) inside the domain; actors
  are fixed for the domain's life.
- Kernel isolation (isolcpus/nohz_full/IRQ affinity) and memory locking stay
  deployment responsibilities (`LinuxRealtimeEnvironment.h`).
