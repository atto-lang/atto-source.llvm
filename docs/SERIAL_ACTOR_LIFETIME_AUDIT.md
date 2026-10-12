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

## Enum lifecycle and TLA+ parity — Oct 11, 2026

The native `SerialActorLifetimeGate::Lifecycle` enum has exactly
`Idle`, `Running`, `Suspended`, `Stopped`. `lifecycle()` is
computed under the same mutex used by `submit/suspend/resume/finish/stop`,
so no second mutable lifecycle variable can diverge from the lease, actor
incarnation, or cancellation fence. The existing `admitted(lease)` versus
`may_execute(lease)` distinction remains unchanged.

Canonical finite-state safety model:
[`SerialActorLifecycle.tla`](https://github.com/ores-truffle-oreslang/oreslang-source.java/blob/integration/serial-actor-runtime-gate-20261011/verification/serial_actor/SerialActorLifecycle.tla).
The same model applies to both runtime admission gates at the abstract
transition layer: one handler lifetime, FIFO queue, exact task provenance,
suspension without admission release, and stopped/restarted incarnations.
[Funded mirror TLC run 38180159775](https://github.com/akrion-sim/akrion-sim-e2e/actions/runs/38180159775)
explored 19,616 distinct abstract states at depth 26 with no violations.
The funded native GCC/Clang sanitizer test compiles the exact C++ header
and checks all four lifecycle phases.

These are **bounded abstraction and native policy tests**, not a complete
LLVM source guest execution or a proof that every physical scheduler path
refines the model. Real HTTP + message admission, resume ticket enforcement
at guest codegen and restart/cleanup conformance are still release blockers.

## Native issuer-instance identity (2026-10-11)

A numeric lease tuple (actor ID, incarnation, generation, event ID, kind)
does not uniquely identify the gate that minted it. Two independently
constructed gates may accidentally have identical public identities after
a failed restart or duplicated supervisor initialization.

The native `Lease` therefore carries a private shared issuer token
created once per `SerialActorLifetimeGate`. Every `admitted`,
`may_execute`, `suspend`, `resume`, and `finish` comparison checks
**both** issuer identity and the complete numeric tuple. Copies of a valid
lease preserve its issuer identity; forged default leases and tickets
from a second same-identity gate are rejected. Shared ownership keeps
the token alive while any stale lease copy exists, avoiding address-reuse
ABA for this identity check.

This provides gate provenance, **not** LLVM scheduler task identity or
end-to-end guest continuation authentication. Native test coverage now
includes duplicate-public-identity gates and fabricated tickets; full
compiled guest/HTTP validation remains a release blocker.
