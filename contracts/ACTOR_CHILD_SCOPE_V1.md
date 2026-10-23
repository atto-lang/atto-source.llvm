# Oreslang lightweight child scope and supervision contract v1

Status: **normative cross-backend target**, not a claim of complete source-level
Java/GraalVM or LLVM implementation. A child scope is runtime metadata, not a
new user-created supervisor, ActorGroup, scheduler, thread pool, or event bus.

## Two independent relationships

1. **Lifetime owner:** An ordinary actor `spawn` creates a child in the spawning
   actor's implicit child scope. The parent owns its child's cancellation and
   **physical completion barrier**, even if it never retains the returned
   `ActorRef`. Nested A -> B -> C inherits the existing managed ActorGroup if
   one exists; otherwise it stays inside the runtime's root lifetime registry.
   No event-bus membership, shared-memory permission, or supervisor object is
   implicitly created.
2. **Recovery authority:** The nearest *registered* supervisor supplies the
   failure policy and receives typed, authenticated terminal CONTROL notices.
   The parent cannot grant itself supervisor policy authority by spawning.
   The default policy is **temporary/no automatic restart**. Actor failure is
   observable through its incarnation-specific completion and supervisor
   terminal notice; ordinary DATA mailbox saturation must not erase CONTROL.

## Spawn, stop, and escape

- Spawn reserves identity, lifetime parent, inherited policy domain and
  supervision registration **before** publishing a routable actor reference.
  Reservation and STOP fences have a single linearization point. If admission
  fails at any stage, retract registration, cancel the partially created actor,
  and retain its cleanup obligation until actual retirement. Never expose an
  unowned live actor as a successful spawn.
- Parent STOP/cancel seals child admission, cancels remaining children
  transitively, then joins **actual child finalization** asynchronously.
  Parent `get_done()` must not settle while a child/descendant still has an
  active carrier, pending continuation, implicit mutex guard, cleanup task,
  FFI/native loan, or actor-owned arena. A clock deadline, cancel request,
  `isStopped`, empty active-worker map, or logical hook completion is not
  physical reclamation evidence. If cleanup fails, report a failed receipt and
  retain/quarantine resources; do not silently free unsafe arenas.
- Ordinary children do not restart after failure. If policy explicitly
  restarts a child, allocate a **new incarnation**; do not reuse its old done
  ticket or mailbox route. STOP fences supersede reserved restart decisions.
- A long-lived independent actor requires an **explicit atomic transfer** of
  lifetime ownership and recovery attachment to an authorized supervisor.
  Transfer must fence the former owner's terminal/join accounting and the
  destination's admissions; refusal leaves original ownership intact. It
  must never silently detach into an orphan or let two scopes own stop rights.

## Terminal receipts and adversarial identity

- Terminal callbacks and restart/cleanup decisions are tagged with:
  `(runtime-id, supervisor-id, actor-id, incarnation, route-epoch, event-sequence)`.
  The frozen parent-child edge also captures the child's incarnation; a
  terminal from A#1 cannot satisfy a join for replacement A#2. Reject foreign,
  future, forged, duplicate and stale receipts at the authoritative CONTROL
  lane. Numeric tokens are **not authentication**: origin capabilities/VM
  registry provenance are checked separately.
- Retain a bounded reserved terminal CONTROL slot before child publication.
  A terminal is a single semantic event, with idempotent processing, and is
  never routed through ordinary user DATA, a best-effort event bus, or a
  potentially full channel without reserved capacity.
- Join observes the physical actor finalizer and a separate release receipt
  for arena/borrow/FFI/continuation resources. Supervisor logical STOPPED is
  distinct from physically RECLAIMED. Any cleanup failure is visible; restart
  and resource reuse remain fenced until required retirement receipts.
- A parent actor's unwind/recover context is not shared with a child. The
  child's panic belongs to supervisor CONTROL, never a cross-actor `recover`.
  No inline guest supervisor callback from a dying child's carrier.

## Layout neutrality and performance

- Keep **one authoritative** bounded parent-child graph and generation
  ledger. It may be encoded as AoS, SoA, AoSoA, SoAoS, or ECS-indexed tables;
  no synchronized shadow copies or heap pointers in CONTROL wire receipts.
- Scope registration is O(1) expected per child; terminal processing is
  bounded by configured per-scope quotas; no thread-per-child/supervisor.
  Aggregation uses asynchronous completion callbacks; never park actor or
  CONTROL carriers to wait for descendants.

## Verification gates (not all implemented)

1. Deterministic spawn-vs-parent-STOP races, including child admission failure
   and rollback, parent physical done delayed by last descendant, and
   cancellation during child cleanup/lease handoff.
2. One-for-one, rest-for-one and sibling recovery only under explicit policy;
   no restarts for ordinary spawn; independent supervised transfer is atomic.
3. Duplicate/stale/foreign/future incarnation receipts, CONTROL flooding,
   mailbox backpressure and restart-vs-STOP, including generation exhaustion.
4. Child throw/raise/panic and cancellation preserve separate typed outcomes
   and exactly-once cleanup; supervisor and parent cannot accept fabricated
   physical reclamation.
5. Exact-source, exact-revision tests across Java/GraalVM and LLVM plus
   source-compiler lowering; sanitizer/race runs for native code.

Existing Java `ActorRuntime` has actor-parent child tracking, implicit
inheritance of an existing managed group and a child-set barrier before parent
finalization. Native `NativeActorGroup` has generation-fenced actor tickets,
physical-finalizer admission and bounded CONTROL. Their complete root
supervision registration, explicit cross-supervisor transfer, source-level
policy lowering, physical loan receipts and differential parity remain
integration work; existing host primitives are not a proof of those features.
