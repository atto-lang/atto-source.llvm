# Serial actor single-flight lifecycle contract (Java/GraalVM and LLVM)

Status: normative cross-backend behavior; the Java/GraalVM branch has a guest
implementation, while LLVM still rejects unsupported guest serial actors.

## Admission and suspension

1. A given actor incarnation admits at most one user message at a time. HTTP
   dispatch and ordinary actor messages share one bounded FIFO user mailbox.
2. A source `await` can release the worker carrier but **not** the actor's
   logical mail-turn ownership. Internal authorized continuations may execute,
   but later user mail must wait until *every distinct completion barrier* has
   retired. The source callback must not execute guest code on producer threads.
3. Each barrier is a runtime-issued one-shot identity. A duplicate completion,
   stale completion, or foreign completion has zero admission authority. A
   stop/restart invalidates all old barrier tickets even when application
   message contents compare equal.
4. Actor state loans do not survive suspension, and no OS monitor or native
   mutex remains held across `await`. Immutable code may be shared; actor state
   remains per-incarnation.

## Transport lifetime, cancellation, and restart

1. A handler registration retains one actor incarnation while it is alive.
   A subsequent supervisor dispatch after termination may create a *new*
   incarnation, with a fresh actor identity and newly initialized state.
2. `handler.done` is a supervisor-only, read-only Future observing the
   currently running incarnation *at access time*. It never grants authority
   to cancel the actor and never silently switches to another incarnation.
3. A request timeout or client disconnect may terminate its **exchange** but
   must not cancel a persistent serial actor serving unrelated messages. An
   expired, queued transfer is consumed as a tombstone without running guest
   receive. Its registry entry is reclaimed on consumption or actor teardown.
   HTTP connection admission and transfer-token admission are independent:
   each server reserves at most min(connection capacity, 1024) unclaimed
   tokens (live or expired). A saturated transfer budget rejects dispatch
   with 503 before emitting actor mail; timeouts do not free these slots until
   the associated mail is consumed or revoked. There is no unsafe eviction of
   tombstones whose actor mailbox messages might still arrive. Supervisor
   counters transfer_backlog, transfer_backlog_limit, transfer_rejected, and
   tombstones expose bounds, overload and cleanup progress; none grants
   guest actor authority.
4. Actor termination fails and reclaims outstanding transport exchanges;
   they are never automatically retried or transferred to a restarted actor.
   Automatic redelivery would duplicate non-idempotent state mutations.
   Higher-level supervisors must define explicit replay/retry policies.
5. When an actor stops, retire all pending barrier identities, remove
   producer-Future callback registrations without cancelling shared producers,
   release actor-owned state, drain mailbox reservations, and fence old
   continuation tickets. A late callback must not revive work or release a
   different actor's turn.
6. `self.forward()` (downstream) and `self.backward()` (upstream) for future
   actor pipelines must enter destination actor mailboxes under these same
   admission/fencing rules. Neither operation bypasses serial scheduling.

## Verification and release boundary

- Finite safety model: `docs/formal/SerialActorTurn.tla` (Java branch)
  and `SerialActorTurn.cfg`; TLC checks single-flight, type/issued-ticket
  integrity, and cancellation fencing for two messages and three tickets.
  This bounded model is not a refinement proof of the full runtime.
- Executable state-machine refinement test: 120,000 seeded hold/complete/
  retire operations against the Java `SerialActorTurnGate`.
- JVM guest: mixed HTTP/mailbox state reuse, concurrent HTTP requests,
  callback cancellation and restart must all run against exact source blobs.
- Graal Native Image: build *and execute* the same serial-actor guest fixture
  in AOT/hybrid, not merely inspect the produced binary.
- LLVM: retain fail-closed guest admission until compiler codegen, scheduler,
  transport, continuation, loan and lifecycle parity are actually executed.
  No runtime garbage collector is implied on the LLVM side.

Do not merge/release based on isolated policy tests or old green mirror runs;
gate each candidate's exact tree and its dependencies.
