# Serial actor native lifetime gate: hardened admission contract

Status: **trusted native runtime substrate; not guest Oreslang compiler
lowering, not an HTTP transport adapter, not an actor scheduler**.

The `SerialActorLifetimeGate` is one instance per supervisor-authorized
actor incarnation. `actor_id`, `incarnation`, event IDs, and generation
tickets are trusted scheduler inputs; never let guest code mint these
identities or access this mutable gate.

## Admission and re-entry

- Exactly one active request or message *lifetime* can be admitted.
  Request and mailbox admissions share the same capacity-checked FIFO
  queue, not separate (1, 1) gates.
- `suspend(ticket)` releases the carrier but **retains** the unique
  logical lifetime until the *same* handler resumes or completes.
- `admitted(ticket)` confirms ownership of that lifetime, even if
  suspended. `may_execute(ticket)` confirms the ticket is both current
  **and runnable**; a suspended ticket cannot enter guest code.
- A transport completion thread must NOT call guest code merely because
  an admission was promoted. It must queue a continuation in the
  scheduler and ensure execution-context identity and critical-state
  loan authority are valid before resuming.
- `finish(ticket)` may promote the next FIFO event only after **all**
  `defer`, `finally`, `recover`, response finalization, loan
  retirement and handler cleanup have actually completed. No native
  mutex should be held across `await`.

## Cancellation, stop and quiescence

`stop()` returns a `StopResult` with `revoked` (active ticket)
and `rejected` (ordered queued event IDs). The supervisor must deliver
a cancellation/failure result to **every** affected request/message
instead of silently dropping it. `finish()` also returns pending
rejections if a generation token cannot be issued and the gate fails
closed. The gate only *revokes* authority; it does not interrupt an
already executing native function or prove quiescence. The supervisor
must wait for physical turn release and all state loans to retire before
reclaiming the actor's storage or starting an incarnation that could
share an address.

No guest callbacks or transport completions run under the gate's
bookkeeping mutex. STOP, full queue, duplicate and stale ticket cases
do not grant new guest admission. Native test coverage checks the
suspend/runnable distinction, 1,000 mixed admissions, queue limits,
actor incarnation fencing, shutdown notification IDs and stop/submit
races.

## Parity and release blockers

Java companion: `SerialActorAdmission` uses a non-recursive future
completion pump so thousands of immediately finishing callbacks cannot
overflow the carrier call stack; it rejects suspended `requireActive()`.
Both languages' gates are only scheduling building blocks. Actual mixed
HTTP + mailbox source integration, generation-fenced supervisor task
tickets, real cancellation unwinding, full LLVM CMake/CTest and GraalVM
native-image end-to-end validation remain mandatory.
