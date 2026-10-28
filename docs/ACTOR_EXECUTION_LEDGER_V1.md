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

## Retained native DATA receipts (new opt-in scheduler path)

The previous bridge exposed a false completion: `on_message(B)` could return
while the guest turn remained queued behind suspended A. The **opt-in native
retained DATA protocol** now addresses the scheduler's accounting portion.

- During an authenticated DATA callback, `retain_current_data()` reserves a
  preallocated slot and returns an immutable ticket
  `(actor ref, actor generation, slot, slot generation, execution ID)`.
  A second retention attempt, non-DATA callback, unauthenticated thread, or
  exhausted pool fails closed without changing the counters.
- When that callback returns, the scheduler leaves DATA B's accepted credit
  and `outstanding` count **unchanged**, and does **not** increment
  `processed_user_messages` or `completed`.
- `queued + active + retained_live` shares the actor's existing bounded
  mailbox capacity. Retained messages are not themselves runnable, and do not
  pin a carrier. A separate admitted WAKE/CONTROL still has its own credit.
- Once a later authenticated physical actor turn has finished B's guest code,
  `complete_retained_data(ticket)` atomically consumes that scheduler credit,
  increments processed/completed, and releases quota. Cancellation after
  *proven physical frame cleanup* instead calls `abandon_retained_data` and
  increments the terminal abandoned counter, without claiming execution.
- Duplicate, stale slot/event generation, forged actor incarnation and
  off-actor completion attempts cannot settle a retained ticket. The precise
  moment the guest frame is physically retired is a **trusted host obligation**;
  the C scheduler cannot inspect compiler-owned guest borrow lifetimes.
- STOP, terminal publication, `wait_done`, `drain`, and `try_destroy`
  remain fenced while a retained receipt is unsettled. A pre-admitted
  supervisor CONTROL cleanup turn may settle one after STOP.

The native integration tests cover two actor lanes with A suspended and B
deferred, independent actor progress, a genuine scheduler WAKE, exactly-once
B completion and a separate C STOP / supervisor CONTROL abandonment scenario.

**Remaining production gap:** Retained-data APIs are a trusted native
scheduler substrate, not a compiler-generated guest dispatcher. The legacy
DATA callback still receives B, and the host bridge manually deposits B in
the actor-local ledger. A future integrated event-ingress ABI must co-admit
the scheduler ticket and actor ledger entry, retain guest frame/heap loans,
dispatch eligible turns without an intermediate guest execution, and detect
cross-ledger divergence as a fatal contract violation.

**STOP liveness gap:** If a retained frame exists but STOP/teardown wins before
a cleanup turn is independently admitted, no new guest callback may be
started merely to discard that frame. The current implementation deliberately
remains nonterminal rather than fabricating quiescence. Add an authenticated,
bounded, actor-owned physical cleanup/retirement path before production or
hard-real-time qualification. Do not use a generic unproven external
force-abandon operation.

## Integration sequence

- Connect an actor-owned ledger to the actual `NativeActorScheduler` lane/actor arena; authenticate every event/continuation token under the same physical actor claim; provide scheduler↔actor revision/slot accounting.
- Route completion, timer expiry, cancellation and stale upgrade tokens through separately bounded ingress; ensure no lost wake while transitioning from SUSPENDED to READY.
- Preserve the existing supervisor CONTROL priority without allowing nested guest state entry; make cancellation cleanup and physical retirement an explicit barrier.
- Differential-test equivalent semantics in Java (PR #161), LLVM, .NET, WASM, Forge and Cranelift. Add a finite-state/interleaving model and a compiler-generated guest source test.
- Keep hard-RT qualification blocked until target-specific priorities, preallocation, WCET, actor-turn blocking and measured code-entry deadlines are proven.

## Evidence

`tests/native_actor_execution_ledger_test.cpp` is a **host-side** C++17 test of these transitions, not compiled Oreslang code. Until executed exact-head native CTest, runner job steps and actual guest lowering are independently verified, this design must remain draft. Do not copy private source across GitHub organizations without explicit export authorization.
