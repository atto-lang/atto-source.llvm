# LLVM hard-real-time target

Status: implementation target. The current LLVM backend is not yet certified hard real time.

The strict native profile is `native-hard-rt` and requires all of:

1. full guest AOT with no interpreter/JIT fallback;
2. no tracing collector or managed heap on the certified execution path;
3. preallocated/bounded memory after admission;
4. a schedulability proof under the selected scheduler and target;
5. bounded blocking, cancellation, cleanup, and actor-region retirement;
6. no unknown FFI or page-faulting growth after start.

The shared diagnostic categories come from `oreslang-common-source/docs/REALTIME_EXECUTION.md`.

## Current substrate

The repository already contains `OwnedHeap`, `EpochArena`, stack-only object lowering, deterministic native AOT for the admitted source subset, and scheduler/cooperation experiments.

These are useful substrates but not a hard-RT proof.

In particular, `OwnedHeap` currently uses `std::shared_ptr` and `std::mutex`. It therefore remains a general native ownership prototype and must not be treated as the strict hard-RT allocator. The hard-RT path needs fixed-capacity/preallocated region storage with bounded synchronization and bounded destruction.

## Initial compiler/runtime boundary

`HardRealtimeContract.hpp` provides a tiny fail-closed host-side admission model for backend work. It deliberately rejects:

- managed heap access;
- tracing GC;
- runtime code generation;
- unknown FFI;
- unbounded allocation/blocking/loops/cleanup;
- possible page-faulting growth;
- missing or impossible timing budgets.

This header is not source-language effect inference. The next step is to lower transitive compiler effects into this admission model and normalize diagnostics to the common contract.

## Required allocator work

The first strict allocator should be a fixed-capacity actor region:

- storage reserved, committed, and touched before RT start;
- bump or segregated fixed-size allocation with bounded execution cost;
- no system allocator in the RT window;
- generation-checked handles;
- explicit quiescence before reset;
- destructors/resources either pre-retired or included in a proven teardown bound;
- fixed-capacity mailbox/task-frame pools drawn from the actor budget.

No source operation may silently fall back to malloc/new when the RT region is exhausted. Exhaustion must be an admitted, bounded failure path.

## Scheduler work

Create a dedicated RT scheduler domain rather than reusing general work stealing.

Initial supported policies:

- fixed priority;
- EDF with admission control.

The RT domain should support preallocated workers, explicit CPU affinity/reservation policy, bounded queues, priority-inversion analysis, and target-specific interrupt/memory configuration.

The existing cooperation watchdog remains diagnostic. It is not a deadline guarantee.

## AOT audit

The release gate must inspect the final linked artifact and transitive runtime/FFI dependencies, not only generated LLVM IR. It must fail if a certified path reaches a tracing runtime, interpreter/JIT, unknown allocator, or unknown blocking primitive.

## Fixed-capacity timer substrate (draft, not wired to guests)

`HardRealtimeTimerQueue<Capacity>` is a distinct single-owner CONTROL
scheduler timer pool; it does **not** replace general `NativeTimerQueue`.
The latter's `std::map`, `std::set`, `std::vector`, and `std::mutex`
operations are not qualified as allocation-free, bounded-blocking RT work.

The strict prototype uses embedded fixed-size slot storage (no per-registration
allocation, mutex, callback, or guest-state pointer) and deterministic bounded
linear scans at each arm/poll (O(Capacity)). It retains READY tickets against
capacity, rejects invalid owner/generation and cross-pool/stale slot tokens,
rejects monotonic-clock rollback and overflow, allows cancellation before
CONTROL execution claim, and surfaces `DeadlineMissed` as a distinct result.
The pool is **not thread-safe**: requests across threads require a *separate*
bounded, preallocated, authenticated ingress path. Its storage must be allocated,
touched, and locked as required before the RT phase. The caller supplies all
monotonic timestamps from the same clock domain.

**Crucial distinction:** `poll_due` merely records expiry; `claim_start`
must be called on the actor carrier **after** acquiring its exclusive actor
turn/lifetime lease, immediately before the first guest instruction, and with
the active actor generation. A late start must produce a supervisor deadline
miss, not silently execute or count as a successful RT turn. Logical callback
cancellation still cannot preempt running guest code. READY messages must not
be queued in a general-purpose FIFO behind unbounded ordinary actor work.

**Admission-budget hardening:** `arm_after(now, delay, actor_id,
actor_generation, maximum_lateness_ns, handle)` now permanently stores the
accepted lateness budget in its timer slot. `claim_start` cannot supply or
retroactively enlarge that budget. The claim returns the admitted bound,
deadline, claim timestamp and observed lateness in a bounded result;
`DeadlineMissed` retires the token without granting user-code execution.
Invalid and stale-generation start attempts do not advance observed clock
time, preventing a forged UINT64_MAX claim from poisoning the timer domain.

The **claim timestamp is not independently the first instruction timestamp**:
carrier preemption or work after the claim can add latency. Certified admission
requires an instrumented compiler/actor-carrier entry seam that samples the
same monotonic clock at the final executable boundary and either guarantees
bounded claim-to-entry interference or reports a miss. Until then this is a
trusted-host admission prototype, not a hard-RT guarantee. A budget fixed at
registration is necessary but insufficient: the runtime still needs a proven
bounded CONTROL queue, actor lease hold time, native timer wakeup, and WCET.

A certified `native-hard-rt` timer additionally requires:

1. A prestarted, isolated RT clock/CONTROL driver using monotonic absolute
   deadlines and an OS/RTOS high-resolution wake primitive (e.g., suitable
   Linux CLOCK_MONOTONIC high-resolution timer or an RTOS hardware timer).
   Never derive deadlines from `CLOCK_REALTIME` or a fixed-rate 1ms wheel.
2. A bounded ingress, timer poll batch, priority/EDF wake lane, and guaranteed
   actor carrier availability. Bound other actors' nonpreemptible critical
   sections and priority inversion. Prove the latency chain:
   clock expiry + wake delivery + ready queue + dispatch + first instruction.
3. A distinct maximum permitted **start lateness** and, where applicable,
   execution completion deadline including WCET, blocking and interference.
   Admission must fail closed when any bound cannot be established.
4. Trace at both planned deadline, actual clock notification, CONTROL enqueue,
   carrier claim, and first guest instruction; report max lateness,
   missed-deadline count, and overrun events under adversarial load.
   Measured maximum alone is *not* a worst-case guarantee or certification.
5. Executed exact-head source-to-LLVM-to-native guest tests with Linux
   RT configuration and target hardware evidence. Host timer unit tests or
   successful simulator/model CI alone cannot certify hard real time.

The fixed-pool CTest target only proves a portion of the host data structure's
functional properties; production guest integration, actor generation leases,
physical retirement, RTOS wakeups, schedulability and measured hardware
latency are still missing. Keep hard-RT timer feature admission **disabled**
until all corresponding requirements are implemented and verified.

## Alignment with Java actor timer code-start contract

The canonical cross-runtime requirement is Java draft
[hard-RT timer/dispatch PR #2210](https://github.com/ores-truffle-oreslang/oreslang-source.java/pull/2210),
with actor lifetime/heap ownership in
[Java #2203](https://github.com/ores-truffle-oreslang/oreslang-source.java/pull/2203).
These are specifications, **not** guest execution evidence. LLVM draft
[#1123](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/1123)
implements only the fixed-pool precursor.

The runtime must distinguish **five** monotonic timestamps, in one comparable
clock domain: `t_due` (absolute release), `t_expiry` (driver recognition),
`t_ready` (validated actor CONTROL readiness), `t_start` (first actual guest
instruction after exclusive actor lease), and `t_end` (physical guest turn
completion). The pool now returns `Expired.observed_expiry_ns` with
`Expired.deadline_ns`; it does **not** provide the later three timestamps.

**Do not mistake `claim_start(..., now_ns)` for a measured first instruction.**
The `now_ns` argument is caller-supplied. Until the compiled guest entry
prologue samples the clock at the actual entry boundary, a returned `Ok`
proves only the supplied-timestamp arithmetic and token checks, not
`t_start <= t_due + B_start`. Any scheduling gap between the sample and
entry must be included in the proven bound. A successful timer claim and
timely callback invocation are not physical retirement receipts.

Before accepting a hard-RT timer, use worst-case schedulability analysis that
includes the existing **non-reentrant** actor handler's maximum remaining
CPU turn, any serialized/critical actor-state guard, all admitted higher and
equal priority ready work, interrupt/wakeup and timer service interference,
CPU/core availability, FFI and bounded cleanup. If an actor can remain in an
unbounded turn, **reject** its hard-RT activation rather than bypass its
single-writer lease. Require a reserved deadline/priority lane and bounded
notification quota separate from ordinary mailbox traffic. Declare a
guarantee only for a specific qualified OS/kernel, CPU, workload and timing
budget; absence of one of those bounds is a fail-closed admission error.

The existing `interval.next()` is **demand-driven fixed delay**, not a
phase-stable hard-RT periodic trigger. Fixed-rate RT periodic activation
needs a separately admitted epoch + period + WCET + start budget and a
bounded skip/fail missed-release policy (never an unbounded catch-up queue).
Timeout observers and timed `select`/channel/critical/network waits may
signal expiration promptly without having a guarantee for when the guest
*handles* that signal.

This draft also fences timer pool identities across different compile-time
capacities, rather than issuing independently restarted domain counters
for each `HardRealtimeTimerQueue<N>` template. Identity fencing does not
establish physical actor isolation, code-start timing or cross-thread
ingress safety.

Required later evidence: deterministic clock-controlled semantics, exact
source AOT binaries, `due/expiry/ready/start/end` traces, worst-case
priority/actor-turn interference analysis, target-kernel `rtla timerlat`
and `osnoise` measurements, and a reproducible missed-deadline fail gate.
Sanitizer/model tests and source-org zero-step workflows are not hardware
deadline proof. Keep draft and keep hard-RT feature admission disabled.

## Actor-local continuation ledger: two-sided dispatch accounting (draft)

The actor must independently decide **which logical turn is eligible next**.
The scheduler owns runnable actor tokens, OS clock registration, priority and
physical carriers; the actor-owned region owns the bounded `ActorExecutionLedger`
with `(actor_id, incarnation, event_sequence)` and turn state. A notification
is **not** an instruction to execute its callback immediately.

The strict default matches Java actor continuation draft #161:

1. The actor acquires its existing authenticated single-writer turn lease;
   only then may the actor-controlled ledger be mutated or inspected. The
   `claim(id, authenticated)` prototype boolean must be supplied only by
   trusted host scheduler integration; it is **not a guest capability**.
2. An `await` suspends the **same** logical message turn and releases the
   physical carrier; later ordinary DATA/mail/timers cannot overtake it even
   if their own readiness arrives earlier. An unresolved timer registration
   is not, by itself, an actor logical-turn barrier.
3. Supervisor/lifecycle CONTROL is an independent urgent lane, but its guest
   handler still obeys exclusive actor state access. No clock driver or
   Future completion thread enters the actor heap or executes guest code.
4. Upon awakening, the actor checks generation, event identity and state,
   then chooses CONTROL first, otherwise the oldest eligible serialized turn.
   Duplicate wakeups and stale cancellations cannot create extra executions.
   An opt-in reentrancy policy is **not implemented**; it will require
   static proof that borrowed/critical actor state is safe across overtaking.
5. STOP fences pending/ready/suspended event identities but retains RUNNING
   physical-turn accounting. `physically_retired()` is one **ledger-only**
   prerequisite, not an OS/actor/FFI resource retirement receipt.
6. An unresolved default-serialized await is an **unbounded deadline blocker**
   absent a separately proven Future completion bound; fail strict hard-RT
   admission rather than claiming a tight callback start. The
   `has_unbounded_suspension_blocker()` query is a necessary negative gate,
   never sufficient evidence for a positive hard-RT admission.

Prototype files: `ActorExecutionLedger.hpp`,
`tests/actor_execution_ledger_test.cpp`, and
`tests/native_timer_actor_double_entry_test.cpp`. The second test crosses
the separate actor-side ledger and scheduler-side fixed timer pool with
deterministic synthetic clock timestamps, verifying a late start is reported
as `DeadlineMissed` instead of masquerading as RT success. Neither test
uses the actual native actor carrier, guest .ores continuation lowering,
OS wakeup thread, or qualified CPU reservation.

**Next integration gate:** wire generation-checked scheduler CONTROL notices
to the actor-owned ledger under `NativeActorScheduler`'s *actual* exclusive
execution authority; reserve finite actor event slots **before** external
timer/producer admission, preserve failed CONTROL notifications until retry,
bind source continuations in `NativeContinuationStore`, and make teardown
wait for the entire native resource/lease retirement barrier. Record real
`due/expiry/ready/guest-start/guest-end` timestamps. This requires an
independent scheduler integration review and source guest execution proof.
