# Actor-local execution ledger V1 — non-reentrant default

Status: **host-only executable contract; actor scheduler integration and guest lowering NOT implemented**.

## Purpose

A scheduler-issued wake is only *eligibility evidence*. It is not authority to run a specific guest callback. Oreslang keeps two cross-checkable but distinct records:

| VM/scheduler-owned | Actor-owned |
| --- | --- |
| Valid actor reference/incarnation, runnable reservation | Exact event sequence and slot generation |
| Physical exclusive carrier lease, domain routing | Current running or suspended logical handler |
| Priority, immutable timer admission budget, RT qualification | Await continuation state, timer callback order, cancellation cleanup |
| Bounded external wake ingress, lane capacity | Bounded actor event slots, deterministic order, lifecycle revision |

A single actor owner serializes local ledger transitions under the authenticated scheduler/actor execution lease. External timer/Future completions must pass through bounded authenticated ingress; they must NOT directly mutate the private actor ledger. The ledger's revision and event identity permit reconciliation without a distributed two-lock transaction. Repeated notifications never create extra guest turns.

## Rules

1. Default **no reentrancy across suspension**: an awaiting handler A remains the logically active handler. A later message B or newly expired callback C can be admitted and recorded but cannot enter guest code while A is suspended, even if A is not yet ready. Once A is ready, A resumes before B and C. No second physical carrier ever executes actor code concurrently.
2. A timer **await** wakes the same event/continuation token. A timer **callback** is a distinct queued event. A supervisor shutdown/cancel fence remains runtime CONTROL (not an arbitrary guest callback).
3. If the actor is suspended and no continuation is ready, do not pin an OS carrier. Scheduler may work on *other actors*. Do not treat idle-carrier availability as permission to run B within this actor.
4. A source-level opt-in reentrancy policy is **not** provided here. If one is considered, it requires a separate compiler proof of state loans, critical guards, async effects, ordering and interference. Current non-reentrant contract does not permit it.
5. Cancellation of a queued handler releases its slot; cancellation of a suspended handler schedules same-turn cleanup; cancellation while running retains its slot and physical ownership until confirmed exit. Logical STOP is not physical quiescence.
6. Opaque event token = `(actor_id, actor_generation, event_sequence, slot_generation, slot)`. Validation is exact; old generations, duplicate wakes, recycled slots and foreign tokens cannot resurrect work. Revision changes only on admitted state transitions.
7. `ActorExecutionLedger<N>` uses inline fixed-size storage, bounded O(N) admission/selection and no per-event heap allocations. **It is intentionally not thread safe.** Capacity, worst-case scan cost, and the scheduler's separate ingress/lease must be proven for a qualified hard-RT profile. Priority CONTROL and deadline misses cannot silently bypass the suspended-handler rule.
8. For hard RT, a timer callback behind an unbounded suspended/nonpreemptible handler is **not schedulable**. Admission must bound actor interference or reject the requested deadline. Measure *due → clock observation → scheduler READY → actor lease → first guest instruction*, not just `claim_under_lease`.

## Await registration/completion race

The previous ledger rejected a completion notice delivered while the handler
was still RUNNING. This could lose a wake between registering a Future and
parking on it. The corrected one-writer protocol is:

1. Under the authenticated actor lease, call `prepare_await(A)` **before**
   exposing the producer completion subscription to external threads.
2. If the actor's bounded completion ingress delivers `wake(A)` before
   `suspend(A)`, store a one-shot early-completion bit.
3. `suspend(A)` returns `AlreadyReady` and retains the current physical
   lease. The caller MUST continue inline, not release or park the carrier.
4. Otherwise `suspend(A)` records A as the sole suspended logical handler.
   Future/timer callbacks enqueue authenticated notices; `wake(A)` on the
   next actor-owned turn makes A ready. A resumes before later queued B.
5. Late duplicate or stale-generation notifications do not create new turns.
   Cancellation before suspend wins and prevents parking.

No external producer may call the actor ledger's `wake` directly; delivery
always occurs on an authenticated actor execution turn.

## Native scheduler lease adapter (host integration, still not production)

`ActorLedgerLeaseAdapter<N>` checks the real native scheduler's
`ores_native_actor_scheduler_is_current_actor` on **every** mutating call,
including wake delivery and cleanup; it fails with `Unauthorized` on
off-actor timer/Future threads. It never caches a lease between callbacks.

`tests/native_actor_ledger_scheduler_bridge_test.cpp` exercises actual
`NativeActorScheduler.c` DATA and WAKE callbacks (two physical workers,
two actors). A suspends; B is recorded but not allowed into guest code;
the independent actor executes; an authenticated wake resumes A, then B.
The original scheduler's generation-checked escrow still linearizes
transfer/publish and rejects forged/duplicate wake notices.

## Retained native DATA receipts and non-reentrant scheduler eligibility

`NativeActorScheduler.c` now has an opt-in retained DATA ticket for the
original running handler A, not just a deferred B callback.

- During authenticated DATA handling of A, `retain_current_data()` allocates
  a preallocated ticket indexed by actor incarnation, slot generation and
  execution ID. The logical A frame remains alive after the C DATA callback
  returns. Its `outstanding` count and bounded mailbox credit stay reserved;
  neither `processed_user_messages` nor `completed` advances.
- Both DATA selection paths in `find_ready()` refuse to dispatch later mail
  while that actor has `retained_live != 0`. The bounded scheduler mailbox
  *still owns B's queued envelope*, including its receipt and quota.
  Supervisor CONTROL and already-authorized continuation WAKE bypass this
  DATA eligibility gate, but never execute alongside another carrier for the
  actor. Independent actors can proceed normally.
- The authenticated WAKE for A resumes and physically exits the same logical
  handler. Only then does `complete_retained_data(A)` consume A's pending
  ticket, record processed/completed and release its credit. The scheduler
  can subsequently dispatch B as ordinary DATA on a **separate actor turn**.
- Cancellation after proven physical A-frame cleanup instead invokes
  `abandon_retained_data(A)`. No off-actor timer/Future producer can perform
  the settlement; stale incarnations, forged slot generations, duplicate ACKs
  and premature host attempts fail closed.
- A separate STOP test admits B while A is retained, observes another DATA
  rejection at full quota, seals the queued B, and uses an already-admitted
  supervisor CONTROL callback to acknowledge A's physical abandonment.
  Terminal counts reconcile processed and abandoned, and cannot be published
  until A's retained credit is settled.

The test bridge is still a **trusted host protocol**, not a compiler-generated
Oreslang guest continuation. The native code cannot independently prove that
the embedding has actually exited its guest frames or released borrows.
The build, sanitizer and concurrent workload checks must execute on the exact
source tree before this draft can be advanced. A bounded scheduler-ready
lane does not yet establish an RT deadline-to-first-guest-instruction proof.

## Pre-reserved actor-owned STOP cleanup (opt-in)

A bootstrap-only `configure_retained_cleanup` callback supplies a dedicated
actor-owned physical cleanup implementation for a suspended continuation.
When the current DATA handler invokes `retain_current_data`, the scheduler
reserves **one additional execution ID** while still under the authenticated
actor lease. No OS thread, heap allocation, user message or guest callback is
created at this point. The reservation exists before the handler suspends.

If STOP or scheduler retirement occurs without an admitted CONTROL/WAKE,
the pre-reserved cleanup credit becomes runnable in a separate lifecycle lane
(ordinary DATA remains blocked). The scheduler acquires the same physical
actor execution authority, invokes `cleanup(ctx, actor, retained_ticket)`,
and acknowledges `abandon_retained_data` **only after cleanup returns 1
and confirms the guest frame and all borrows physically unwound**.
A successful STOP subsequently reaches a genuine terminal receipt;
a concurrently running DATA callback must release its physical lease first.

Internal cleanup is a **pre-admitted physical lifecycle execution**, not a
supervisor CONTROL envelope. It contributes to global `accepted` and
`completed` once its callback returns, but must never increment the public
`control_completed` counter. Genuine supervisor CONTROL callbacks are
counted independently. The STOP, cleanup-only and STOP/wake race regressions
now assert this split explicitly.

Normal Future completion cancels the unused cleanup reservation without
incrementing `accepted` or falsely reporting that the cleanup ran. Global
acceptance accounting reserves numeric headroom for every possible cleanup
callback, so unrelated DATA/CONTROL/WAKE admission cannot exhaust the
`uint64_t` counter before a pre-admitted STOP cleanup turn runs. The
`native_actor_retained_cleanup_unused_credit_return` regression proves
that normal resumption releases this reservation, produces no cleanup
callback, and reconciles accepted/completed/terminal counts.
If a previously admitted supervisor CONTROL retires the frame, it can also
cancel an unclaimed cleanup reservation under the actor lease.

If cleanup returns 0, the native scheduler marks `cleanup_failed`, wakes
host waiters, and makes `wait_done` / `drain` return false. The retained
frame stays quarantined and `try_destroy` stays forbidden. No retry is
implicit and this is **not** a proven bounded physical drop or a completed
actor lifecycle. A time bound for the callback is a separate target-specific
admission/WCET obligation, not supplied by this host implementation.

The `native_actor_prepaid_stop_cleanup_and_failure_quarantine` regression
contains a STOP-with-no-prior-CONTROL/WAKE success path and an explicit failed
cleanup quarantine path. ASan/UBSan run the success-only path because the
failure path intentionally cannot destroy its quarantined retained frame.

Without an installed trusted cleanup callback, the older fail-closed
nonterminal behavior is retained. No ordinary guest-code reentrancy is
introduced by this lifecycle mechanism.


## Cross-scheduler wake admission (deny by default)

The native scheduler's `reserve_wake`/`transfer_wake` path is a **local**
mailbox-credit and producer-lease protocol. It has no authenticated
cross-scheduler target-admission bridge. Previously a foreign
`target.scheduler_domain` was accepted at reservation and excluded from
local target-liveness verification at transfer. This could produce an
irrevocable producer credit without proof that the foreign target was ever
admitted, allowing a false cross-domain delivery claim.

The current implementation **rejects foreign scheduler-domain targets at
RESERVE**, and checks the exact live local target incarnation at TRANSFER.
The native cleanup-reservation regression additionally requires a forged
local generation and a foreign scheduler domain to leave the wake escrow
empty and `accepted` unchanged; after normal wake completion it rejects
transfer, cancellation and republication of the stale ticket. No new
cross-scheduler API has been added. A future bridge must authenticate both
domains, exact target generation and actual destination mailbox acceptance
before granting an irrevocable delivery receipt; until then it fails closed.

### Local target STOP between transfer and publication (#1153)

The local target can retire **after** a successful TRANSFER but **before**
PUBLISH obtains the scheduler lock. TRANSFER does not make a future target
generation permanently live and a producer wake is not a target delivery
receipt. PUBLISH therefore rechecks the exact target incarnation under the
scheduler lock immediately before it commits the preallocated wake credit.

If the **producer** has stopped, PUBLISH releases its transferred escrow
and returns `ORES_NATIVE_WAKE_PRODUCER_RETIRED`; it does not execute guest
code. If the **target** retired while the producer remains live, PUBLISH
returns `ORES_NATIVE_WAKE_TARGET_RETIRED` and queues a single
`ORES_NATIVE_WAKE_CANCELLED` callback on the producer's authenticated
actor lane. The callback, not the completion thread, may physically settle
a retained message and release its actor-owned loans. This callback increments
the scheduler's own `accepted/completed/wakes_completed` execution counters:
**it does not imply the target accepted or processed a message**. A normal
`ORES_NATIVE_WAKE_PUBLISHED` only proves that the producer-side wake was
queued while the target was live at the publication linearization point;
it is not proof of any later remote/mailbox delivery.

The deterministic same-scheduler two-actor regression in
`tests/native_actor_retained_stop_wake_race_test.c` now includes STOP of
the target B before TRANSFER, STOP of B after TRANSFER before PUBLISH, STOP
of producer A after TRANSFER, and STOP of **both** producer and target before
publication (producer retirement takes precedence without a wake callback
or accepted execution credit). It also exercises a retained DATA frame
when the target STOP occurs before TRANSFER: the producer must explicitly
cancel its old escrow and schedule a *new local cancellation wake* to unwind
the retained frame; denying TRANSFER alone cannot complete a Future. The
post-TRANSFER test verifies that the automatic typed cancellation callback
settles the original retained DATA and that an old ticket cannot reclaim a
recycled wake-credit generation. The concurrently exercised version runs
48 target STOP/PUBLISH interleavings on separate threads. Each case asserts
exactly one producer wake outcome — PUBLISHED/READY if publication linearizes
first or TARGET_RETIRED/CANCELLED if STOP linearizes first — no accepted
target DATA receipt, no wake-credit leak and exact execution accounting.
An outcome already published while the target was live is not retroactively
rewritten by a later STOP; the producer wake is distinct from target delivery.
The previously existing 80 STOP/wake
interleavings for a same-actor target remain intact.

These regression cases have been committed to the original private LLVM
source branch but require an executing, authorized runner before they may
be reported as passing.

These cases are committed source-level assertions, **not** runtime passes:
the source organization's latest Actions attempts still have zero executed
steps. An alternate private-org source-free toolchain canary tests only the
funded runner and does not authorize transfer of this private LLVM tree.

## Deterministic STOP-versus-wake linearization matrix

`tests/native_actor_retained_stop_wake_race_test.c` adds 80 actual
native-scheduler iterations divided among four synchronization modes:

1. Both producer and STOP call race concurrently.
2. STOP wins before transfer: the reserved wake is cancelled, and the
   pre-reserved actor-owned cleanup retires A.
3. Transfer and publication precede STOP: an already-admitted WAKE can
   legitimately finish A; the unused cleanup reservation must be cancelled.
4. Transfer precedes STOP, then publication occurs: the producer is retired,
   no guest WAKE executes, and the retained frame is physically cleaned.

Each mode asserts one and only one final retirement path (WAKE completion
or certified cleanup), the DATA accepted = processed + abandoned terminal
identity, stale duplicate ticket refusal, and no overlapping actor guest
entry. It runs under strict GCC/Clang and Clang ASan/UBSan CI configurations.

**Important:** These regressions are committed but remain **unexecuted on
this latest native source head** until a runner executes real compile/test
steps. Scheduling four forced interleavings is not a proof of all possible
interleavings or a hard-real-time WCET bound. The implementation remains draft.

## Compiled native guest-segment bridge (intermediate, not actor-await lowering)

`tests/fixtures/native-retained-guest-steps.ores` contains four real
Oreslang functions (`guest_step`, `guest_resume`, `guest_cleanup`, `main`). The CTest
`source_actor_compiled_native_retained_guest_bridge` and its sanitizer variant
run `oreslang-llvmc` on these exact source bytes, verify that the generated
LLVM IR defines executable `i64` symbols and no tracing-GC calls, rename only
the guest module's `main`, then link its native machine code with
`NativeActorScheduler.c` and
`tests/native_actor_compiled_guest_ledger_bridge_test.c`.

A's first DATA executes compiled `guest_step`, reserves a Future WAKE and
retains **A's original DATA receipt**. B remains queued, and actor C executes
its own compiled guest step. Before WAKE, the bridge requires global accepted
= 3 and completed = 1 (C only), A retained = 1 and B not executed. An
authenticated WAKE executes compiled `guest_resume`, settles the original
ticket exactly once, then the scheduler dispatches B to another authenticated
DATA turn. The bridge now uses **two distinct physically allocated**
`OresActorI64Heap` instances, not actor-indexed scalar fields in a shared
host fixture. Each compiled guest function operates on an i64 loaded from
its authenticated actor's own native heap. Actor A's heap slot is
shared-read-loan pinned before the first retained DATA callback returns;
the loan persists across the native suspension, and only the authenticated
WAKE releases it before writing A's resumed value. Actor C's independent
native heap progresses while A remains pinned. Expected diagnostic
snapshots: A = 1256 and C = 9. After initial drain, accepted=completed=4.

The test then STOPs A, verifies terminal quiescence, and **physically
destroys A's actor-owned heap** while actor C's scheduler lane and separate
heap stay alive. It dispatches a second compiled guest message to C and
requires its state to advance from 9 to 108, with global
accepted=completed=5. Only after C's own STOP does it retire C's heap.
This is stronger than mere logical actor-indexed state separation.

A second isolated scheduler fixture also exercises **STOP without any prior
WAKE**. The trusted host allocates a separate native `OresActorI64Heap`
for this actor. The first DATA turn executes compiled `guest_step`, creates
an owned heap slot, takes a shared loan, retains the original DATA receipt,
and physically exits while the loan remains pinned. A supervisor seals STOP.
The pre-reserved, authenticated actor cleanup turn re-enters that same heap,
reads the pinned slot, and executes *compiled* Oreslang `guest_cleanup`.
Only after that guest function exits does the trusted cleanup turn release
the loan, leave/retire the arena and **physically destroy the native heap**.
Only then may it acknowledge retained-DATA abandonment. The test checks exact
ticket/generation, one cleanup turn, the physical release flag, one abandoned
user message, zero processed messages, accepted=2, completed=1, no phantom
supervisor CONTROL receipt and terminal/quiescence. Both paths are registered
for release and ASan/UBSan exact-head CTest/Actions verification.

The native heap and its loan/drop protocol are still **host-managed** in this
test. This is evidence of the intended native runtime ownership boundary,
NOT proof of compiler-lowered suspension, automatic generated defer/drop glue,
or guest source-level actor isolation. In addition, the exact-head workflow
now runs existing standalone native actor-heap misuse rejection and pinned
continuation cancellation tests under GCC, Clang, and Clang ASan/UBSan. These
checks must execute before any claim that the STOP/loan changes passed.

A native C test runner cannot implement the Oreslang compiler's missing
source-level actor continuation lowering. This gate therefore exercises
**actual compiled guest scalar code under trusted host DATA/WAKE control**, not
a compiled `suspends on_message` state machine, source-level suspension
frame ownership, or native actor heap allocation by the language compiler.
Heap opening, actor-ID mapping, retain/loan handoff, and heap teardown
remain performed by the **trusted host test bridge**. These native heaps
are independent allocations, **not** separate process address spaces,
OS sandboxes or independently tracing JVM heaps.
The test explicitly checks that
`source-actor-suspend-method-reject.ores` still fails closed with the existing
unsupported-continuation diagnostic. When guest continuation lowering is
implemented, replace that rejection guard with a genuinely compiled positive
source-actor-await test and require correct guest-state physical reclamation.

The additional `compiled-ores-guest` GitHub Actions job uses a private
exact-PR-head checkout, checks source Git blobs, builds the actual LLVM
compiler and executes 12 optimized and 3 sanitized isolated schedules.
This is **defined CI**, not a claim of completed runs: GitHub runner admission
and nonzero job steps must be verified on the final immutable head.

## Integration sequence

- Connect an actor-owned ledger to the actual `NativeActorScheduler` lane/actor arena; authenticate every event/continuation token under the same physical actor claim; provide scheduler↔actor revision/slot accounting.
- Route completion, timer expiry, cancellation and stale upgrade tokens through separately bounded ingress; ensure no lost wake while transitioning from SUSPENDED to READY.
- Preserve the existing supervisor CONTROL priority without allowing nested guest state entry; make cancellation cleanup and physical retirement an explicit barrier.
- Differential-test equivalent semantics in Java (PR #161), LLVM, .NET, WASM, Forge and Cranelift. Add a finite-state/interleaving model and a compiler-generated guest source test.
- Keep hard-RT qualification blocked until target-specific priorities, preallocation, WCET, actor-turn blocking and measured code-entry deadlines are proven.

## Evidence

`tests/native_actor_execution_ledger_test.cpp` is a **host-side** C++17 test of these transitions, not compiled Oreslang code. Until executed exact-head native CTest, runner job steps and actual guest lowering are independently verified, this design must remain draft. Do not copy private source across GitHub organizations without explicit export authorization.
