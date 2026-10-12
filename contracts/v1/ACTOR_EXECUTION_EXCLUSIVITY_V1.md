# Oreslang v1: exclusive physical execution of every actor

**Normative language/runtime invariant** — applies to private, shared, untrusted,
serial, reentrant and supervised actors on Java/GraalVM, LLVM/native, browser and
any future runtime backend.

An actor is identified by its **runtime domain + actor slot + incarnation
generation**, not by a carrier thread, group membership or source type name.

## 1. Physical exclusivity

For every live actor incarnation `a` and real instant `t`:

`cardinality({ worker | worker is physically executing actor-owned code for a at t }) <= 1`.

Actor-owned execution includes **all** user-defined actor handlers and methods,
actor constructor/initialization hooks, `receive`, actor-local `on_throw` /
`on_raise`, `tear_down` / cleanup, suspended source task continuations,
timers/HTTP/stream callbacks dispatched into the actor, supervisor CONTROL
callbacks when that actor is the supervisor, and native/FFI callbacks into an
actor's guest execution context.

A callback may be invoked synchronously by a current admitted turn without
introducing a second executing **thread**. It must not create a second
independent carrier claim. A foreign thread must enqueue a callback, not call
actor code directly.

The scheduler may reuse a different thread on a subsequent turn. No permanent
one-thread-per-actor or one-scheduler-per-actor policy follows from this rule.

## 2. Reentrant versus serial

- **Reentrant actor:** multiple logical handlers may exist and may await.
  Only one handler segment may be actively executing actor-owned code.
  On suspension the physical carrier and exclusive physical lease are
  relinquished. Another queued handler/continuation can acquire the free
  lease. The resumed handler must acquire the lease again before actor code.
- **Serial actor:** additionally retains exclusive logical *mail admission*
  for the handler across suspension, even while its physical carrier returns
  to the scheduler pool. It does not hold a native mutex or pin an OS worker
  while suspended.

Neither mode permits simultaneous execution of two actor methods on
different threads. `critical` protects additional logical invariants but does
not replace or relax actor-wide physical exclusivity.

## 3. Authorization/refinement

- One trusted runtime authority per actor incarnation decides physical
  admission. Other scheduler pools, actor groups or supervisors must not mint
  a second lease for that actor. ActorGroup membership grants no execution
  authority.
- The indivisible claim checks an unoccupied lane and records its executing
  owner before entering actor code. Release must be by the **same physical
  owner**, after callback completion and interpreter/native context unwind.
- Wakeups, timers, event-loop completions, HTTP transport completion and future
  resolution only enqueue work. They must never directly run an actor callback
  on producer threads.
- Stop/cancel fences fresh admissions and stale actor generations, while
  already-admitted callbacks unwind within their current single lease.
  Teardown and cleanup themselves require that lease.
- A supervisor has its own actor identity and its CONTROL + DATA handlers
  share that same lease. A child cannot run a parent CONTROL callback on the
  child's physical worker.
- ActorGroup-owned Mailman/control code is separate from the creator actor's
  execution, and must never dereference creator actor-owned mutable state
  without entering that actor through its own mailbox.
- Unsafe native/FFI code, embedded interpreters and foreign threads must
  preserve this invariant. Code that spawns a thread to access actor-owned
  state directly is outside the safe actor programming model and must be
  rejected or explicitly isolated at the FFI boundary.

## 4. Verification

The companion `docs/formal/ReentrantActorExclusiveCarrier.tla` and
`scripts/verify_actor_carrier_exclusivity.py` model logical awaits with
exclusive physical claims. The finite Python model enumerates 372 states with
no overlap; its guard-removal mutation reaches an overlap in 65 explored
states. These facts prove **the abstract protocol only**.

Production runtime refinement additionally requires tests and proof for
initialization, message handling, resumed continuations, lifecycle cleanup,
supervisor CONTROL, FFI, browser Workers, cancellation and actor-group
handoffs. The Java runtime's `scheduled` and `activeTurns` checks and
the native runtime's `ActorLane.active` are the actual serialization
boundaries. Explicit thread-owner validation is a defense-in-depth check;
the compiler must not treat a syntactic `serial`/reentrant declaration as
proof that an unverified native callback is safe.

Do not claim a machine-checked Java/LLVM implementation proof until TLC
has run and all concrete entrypoints have a checked correspondence to the
formal transition system.
