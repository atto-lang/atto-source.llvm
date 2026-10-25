# NIO and physical actor-turn semantics — portable v1 contract

Status: **normative target, not a statement of feature completion**.
A backend that cannot implement an operation safely MUST reject it explicitly.
Partial host primitives are not authority to execute source actors concurrently.

## I/O and scheduling

1. I/O guest operations MUST be asynchronous and never block an ordinary
   actor, CONTROL, or root execution carrier. A pending operation suspends the
   logical task and returns the carrier to its runnable-work scheduler.
2. A dedicated reactor MAY wait only in its kernel readiness/completion API
   (for example epoll, kqueue, IOCP, or an appropriate Wasm host event loop).
   Reactor callbacks perform bounded host bookkeeping only. **Guest code must
   execute on its owning actor or CONTROL scheduler, never on a reactor.**
3. The normal throughput profile targets **zero VM-owned reactor threads at
   cold startup**. Reactor/scheduler descriptors and bounded queues MAY be
   constructed eagerly, but the first valid readiness admission lazily starts
   reactor #0. Sustained measured reactor pressure may grow the reactor set up
   to a bounded implementation maximum (initial target: four). Pending handle
   count alone is never a scaling signal. An explicit warm/server profile MAY
   prestart one reactor; that is configuration, not a language semantic.
4. PRIVATE, SHARED and UNTRUSTED execution carriers are demand-driven unless
   a backend/profile explicitly chooses a warm minimum. CONTROL MAY be
   prestarted as a VM bootstrap guarantee. The number of resident CONTROL
   workers is an implementation/profile property, not a source-language
   semantic; whichever policy is chosen MUST be bounded and observable.
5. Canonical normal-profile completion isolation adds **no unbounded private
   completion-worker population**. A reactor publishes only bounded immutable
   readiness/completion records; continuation resumption is enqueued to the
   owning scheduler/domain, or to an explicitly supplied host executor for
   host-only callbacks. Reactor threads MUST NOT execute arbitrary Future
   subscribers, and slow subscribers MUST NOT consume/starve CONTROL. A
   transient bounded bootstrap helper is conforming only when it never runs
   guest completion code and is included in physical-worker accounting.
6. When a backend advertises a VM-wide physical execution-carrier budget, that
   budget MUST be actually enforced rather than inferred from independent pool
   sizes. CONTROL retains guaranteed progress capacity and every additional
   host execution domain included in the claim must consume the same permit
   authority. Pending I/O, actors, ActorGroups, mailboxes and suspended
   continuations cannot consume physical-carrier credits merely by existing.
7. An **explicitly user-created fixed scheduler** is a deliberate exception to
   ordinary VM-pool laziness. Where exposed (for example Java
   `OresScheduler(N)`), `N` means exactly `N` dedicated user-owned carrier
   threads, created/prestarted because the user explicitly requested them.
   Such a scheduler is never created implicitly per actor or ActorGroup and
   its carriers are accounted separately from the normal VM cold-start
   invariant. A backend without this surface must not emulate it with hidden
   per-object threads.
8. The strict profile forbids blocking-worker/virtual-thread I/O offloading.
   Unsupported socket/file/HTTP/stdio operations reject; they must not silently
   use blocking calls or pretend that an async-looking API is OS-nonblocking.
9. Socket, read/write operation, continuation and completion records are owned,
   bounded, generation-fenced and cancel-safe. A descriptor is admitted by at
   most one active I/O owner; a stale completion cannot settle a reused slot.
10. A VM/guest may use existing OS/JVM/.NET/host event loops when dedicated
   reactor ownership is unavailable, but MUST verify that its guest operation
   is genuinely nonblocking. It MUST NOT claim explicit 1..4 reactor-thread
   control if that platform does not expose it.

## Per-actor execution and logical concurrency

Source actors are **physically single-flight**. At most one native thread or
carrier may execute guest code for a given actor at any instant. Suspension may
release the carrier and the continuation may later resume on a different
carrier, but two turns for the same actor never execute guest code
simultaneously.

Three distinct quantities MUST NOT be confused:

- Logically outstanding request lifetimes;
- Logically outstanding message lifetimes;
- Physical execution across *different* actors in an ActorGroup or VM.

`requestConcurrency` and `messageConcurrency` bound outstanding lifetimes,
not simultaneous native execution of one actor. `serial actor` is equivalent
to `(requestConcurrency=1, messageConcurrency=1)`. A suspended message may
allow another admitted lifetime to make progress, but the scheduler must still
preserve one physically executing guest turn for that actor.

ActorGroup physical parallelism is an aggregate bound across different actor
instances. It must never be reinterpreted as permission to execute one actor on
multiple carriers concurrently. Host-only credit gates, worker-pool sizes and
legacy `MaxActorThreads` metadata are not source authority for intra-actor
parallel execution.

Actor persistent mutable data lives only in the actor's single protected
`state { ... }` schema. Access occurs through a verified lexical critical
scope such as `critical handle_state(callback)` or the equivalent state
read/write scope. The logical critical scope/ticket MAY span a correctly lowered
suspension while retaining exclusion, but a raw/native mutable `StateAccess`
borrow or projected state-backed reference MUST NOT remain live across
`await`, blocking select/channel I/O, `rt cooperate`, task handoff or an
equivalent suspension boundary. The implementation must revoke/release that
borrow before suspension and revalidate a fresh access view on resume under the
same logical critical ownership. While exclusion is retained, another admitted
message lifetime may execute non-state work, but competing state-scope admission
remains blocked. No state-backed view may escape its lexical callback, be copied
to another actor, or survive actor termination.

A backend that cannot preserve these invariants MUST reject the relevant source
feature before guest execution. No backend may silently downgrade logical
concurrency into parallel execution or silently reinterpret a suspended state
loan as released.

## Native real-time profile

LLVM and any native RT implementation target zero tracing-GC and deterministic
bounded native lifetimes. Preallocate fixed carrier/reactor sets and bounded
tickets/arenas BEFORE admitting deadline-sensitive work. Do not grow pools,
allocate unboundedly, or block on ordinary locks on the certified hot path.
This alone is **not** a hard-real-time guarantee: platform-specific WCET,
preemption, scheduler priority and device/kernel latency proofs are required.
Backends without proven RT admission must explicitly deny #hard.

## Current implementation classification (2026-10-24)

| Backend | NIO status | Actor execution | RT status |
| --- | --- | --- | --- |
| Java/GraalVM | Current main prestarts bounded CONTROL; VM readiness starts reactor #0 lazily. Full OresContext binds terminal notifications to actor carriers, with at most one short-lived off-selector bootstrap helper for a cold bound executor. Standalone readiness retains a bounded lazy 0..2 completion pool. Legacy HTTP/FS paths reject in strict NIO mode | Source actors are physically single-flight. request/message concurrency may admit multiple outstanding lifetimes, but never simultaneous guest execution for one actor. Legacy LOW metadata is compatible with this; MEDIUM/HIGH must reject | Hybrid GC; no hard-RT guarantee |
| LLVM | Merged PR #880 provides Linux epoll VM lifecycle integration, generation-fenced completion/terminal accounting and fixed realtime prestart; later drafts add structured cancellation, wake escrow, physical loans and readiness/Future work | Source actors remain physically single-flight. Native host credits and ActorGroup parallelism may schedule different actors concurrently but are not authority for intra-actor parallel turns | Zero tracing-GC substrate and fixed prestart profile; no WCET guarantee |
| .NET | Source compiler/runtime has no guest NIO implementation | No certified guest actor execution parity; unsupported source actor execution must reject rather than emulate parallel turns | Managed GC; no hard-RT claim |
| WASM | Zero-import scalar guest; no guest NIO or reactors | No guest actor execution; reject actor execution requests | No hard-RT claim |
| Forge | Mailbox prototype; no guest NIO/reactor | No certified source actor execution parity; host-only credits are not source authority | Native/no tracing-GC host credit path; no hard-RT claim |

## Incremental Java enforcement (current branch)

- Current Java main's CONTROL startup barrier is preserved. This branch does
  not claim zero resident CONTROL workers.
- `OresVM` owns one lazy `NioReadinessGroup`. The first accepted watch starts
  reactor #0; pending descriptors alone do not create actor carriers.
- In a full `OresContext`, NIO completion delivery is bound before first watch
  to `ActorRuntime.executeHostIoCompletion()`. Selector-originated submission
  may target only an already-started carrier through a proven no-growth API.
  The first runnable completion may use one short-lived off-selector helper
  solely to bootstrap the cold actor executor; the completion itself runs on
  actor-carrier capacity.
- `OresContext.close()` drains NIO before actor dispatchers retire.
  `OresVM.close()` rejects self-join from scheduler/actor/CONTROL/NIO worker
  execution and aggregates teardown failures rather than skipping another
  owned domain.
- `-Dores.runtime.carriers.global.max=N` accepts **5..256** and fails closed
  unless `ores.runtime.carriers=java` and `ores.actor.affinity=none` are
  explicit. In that topology a single permit coordinator covers CONTROL,
  PRIVATE, SHARED, UNTRUSTED, and LIFECYCLE/bootstrap host workers. Each domain
  retains one protected reserve; JVM non-affine domains may borrow idle permits
  without consuming another domain's final reserve.
- Dedicated kernel readiness reactors and explicit user-owned
  `OresScheduler(N)` carriers are separate resources and are not included in
  the execution-carrier budget.
- The LIFECYCLE permit domain currently accounts the transient NIO
  bootstrap/completion helper. This branch does not implement #1100's separate
  lifecycle-observer/terminal-journal subsystem.
- The global budget is deliberately unavailable for native/affine actor
  topologies until their individual carrier start/retirement lifecycle can be
  tied to the permit coordinator without speculation.
- Source `MaxActorThreads.LOW` may be accepted for compatibility because it
  denotes the canonical single physical guest turn. `MEDIUM/HIGH` must reject:
  request/message concurrency does not authorize intra-actor parallel execution.
- Java exact-head JDK/GraalVM execution is still required before merge.
  Zero-step source-organization Actions failures are not test failures.

## Actor-system reliability and operability invariants

These are cross-backend semantic requirements. A backend may explicitly reject
an unsupported feature, but it must not silently weaken these guarantees.
Several items below (notably terminal journals and dedicated lifecycle-observer
workers) are future/other-slice requirements and are **not implemented by this
Java NIO/carrier branch**.

- **Bounded overload, never hidden growth.** User mailboxes, continuation
  headroom, dispatcher ready queues, lifecycle-observer work and diagnostic
  journals are bounded independently. Saturation returns a typed/reasoned
  admission failure or increments an explicit drop counter; it never creates
  one thread per actor/message/socket or silently expands an unbounded queue.
- **CONTROL and failure progress are independent of user work.** A saturated
  DATA domain cannot consume the final CONTROL progress permit or require a
  parent actor mailbox turn merely to make child failure visible. Stop,
  cancellation, panic and terminal parent bookkeeping must have bounded
  progress paths under ordinary DATA saturation.
- **Post-mortems survive telemetry loss.** Every actor incarnation retains one
  immutable terminal state on its ActorRef/capability after authoritative
  finalization. A bounded global terminal journal may drop records under
  pressure, but drops are counted and cannot erase the per-ref post-mortem or
  delay actor/parent finalization.
- **Failure-time diagnostics cannot poison themselves.** A terminal journal
  must not expose a producer-reserved-but-unpublished head slot that can hide
  all later events if the producer dies. Drain order must be monotonic for
  fully published records, sequence numbers must never wrap, and diagnostic
  publication must never invoke user Throwable formatting synchronously.
- **Callbacks are not lifecycle authority.** Arbitrary ready/DONE/output/exit
  observers never run while actor lifecycle locks are held and never execute
  on actor, CONTROL or selector carriers merely because another executor is
  saturated. Parent structured-finalization progress and callback-free
  termination barriers precede arbitrary observer callbacks.
- **Operational snapshots are race-tolerant and non-mutating.** Implementations
  should expose logical mailbox state separately from physical scheduler
  pressure (started/active carriers, ready backlog, bounded capacity and
  lifecycle-observer backlog). Sampling concurrently changing counters must not
  itself throw merely because two fields were observed at different instants.
- **Restart policy belongs to supervision, not failure loops.** OTP-like
  supervisors must bound restart intensity over a time window and support
  backoff/jitter or an equivalent policy. Generation/incarnation identity must
  make stale messages, monitors, timers and remote replies unable to target a
  replacement actor accidentally. Blind immediate infinite restart loops are
  non-conforming.
- **Finalization is idempotent and generation-fenced.** A stop/failure/cancel
  race publishes at most one terminal state per actor incarnation, drains or
  accounts for every admitted envelope/continuation, and cannot resurrect a
  retired execution domain through a late Future/channel callback.

## Actor lifecycle Future / HasFuture semantics

`ActorRef<T>` is awaitable through its readiness projection. Direct
`await actor` is semantically equivalent to awaiting `actor.get_ready()`;
it is **not** a shorthand for `get_done()`. `get_done()` is the distinct
physical-quiescence/finalization barrier.

Lifecycle observation is non-consuming:

- `get_ready()` and `get_done()` expose runtime-owned observation Futures.
  Awaiting one observer does not consume the ActorRef or make the lifecycle
  result unavailable to a later observer.
- Repeated readiness observation of the same live actor must report the same
  readiness outcome. Repeated DONE observation after finalization must report
  the same terminal outcome.
- A backend may temporarily support only one lifecycle await site or a single
  compiler-owned `Future<void>` token, but that is an implementation
  restriction and MUST fail closed with an unsupported-feature diagnostic. It
  must not be presented as a language-level linear/one-shot lifecycle rule.
- A source Future or continuation may never execute guest code on the producer,
  selector or arbitrary completion thread. Completion publishes state and
  schedules the owning logical continuation through bounded scheduler/CONTROL
  admission.
- Stop-before-ready fails readiness with the actor-termination effect; DONE
  still settles only after physical actor finalization. Cancellation and stale
  generation/incarnation callbacks cannot revive either observer.
- No raw actor-state borrow, mutable `StateAccess`, native pointer or
  state-backed projection may be retained merely because a Future remains
  observable. Suspension follows the state-loan rules above.

General `Future<T>` lowering may remain narrower on a backend, but lifecycle
Future semantics and direct ActorRef readiness must not be weakened to match a
backend's temporary compiler slice.

## Required differential fixtures

- Java current-main startup: bounded CONTROL is prestarted; readiness reactors
  and NIO completion helpers remain zero until demanded. The first valid watch
  starts exactly one reactor. Pending watches alone create no actor/bootstrap
  carrier.
- A first runnable bound Java completion may create one transient off-selector
  bootstrap helper. With global budgeting enabled that helper consumes the
  LIFECYCLE permit domain. The callback executes on actor-carrier capacity and
  the helper retires after handoff.
- Warm affine and non-affine Java actor carriers accept selector-originated
  host completion records without worker growth.
- Under the optional JVM/non-affine global budget, concurrently runnable work
  never exceeds the permit ceiling and borrowing cannot consume another
  domain's final reserve. All permits retire only after physical worker exit.
- If a backend exposes a fixed user scheduler, constructing it with `N`
  explicitly creates exactly `N` user-owned carriers; ordinary actors and
  ActorGroups never receive dedicated OS threads implicitly.
- Concurrent connect/accept/read/write, partial writes, EOF, cancel/close
  races, generation/descriptor reuse, ownership and bounded backpressure.
- A reactor publishes completions only; guest callbacks run on the owning
  scheduler. Idle/reactor shutdown never strands accepted operations.
- Every source actor remains physically single-flight. Legacy LOW metadata may
  express that invariant; MEDIUM/HIGH must reject rather than enable parallel
  guest turns. Multiple request/message lifetimes may remain outstanding and
  interleave only at suspension points under the one-turn rule.
- Actor lifecycle differential coverage must prove `await actor` ==
  `await actor.get_ready()`, distinguish `get_done()` as physical
  finalization, and exercise repeated observation of readiness/DONE without
  consuming the ActorRef or lifecycle result.
- Explicit unsupported-feature denial on .NET/WASM/Forge until source lowering
  exists. Host-only credit/NIO tests do not count as source guest success.
- LLVM RT qualification must retain zero tracing-GC, preallocated hot-path
  resources, bounded admission, clear denial of unproven #hard, and a thread
  census showing no post-admission runtime-thread creation.

## Evidence and source handling

Record immutable PR head commit, source Git tree, exact tested blob/tree,
runner job/step execution, compiler flags, and source-level guest tests.
A zero-step GitHub Actions failure proves neither pass nor fail. An exact-blob
host test establishes only that slice; it is not full source-tree parity.
Private cross-org source transfers require a reviewed protected-main approval
for the specific private destination, independent of Actions minutes.

## Legacy MaxActorThreads compatibility

Some staged branches recognize the following language-owned enum for parser and
diagnostic compatibility:

```oreslang
define enum MaxActorThreads as
    LOW int = 1
    MEDIUM int = 2
    HIGH int = 3
end
```

This metadata does **not** override the canonical actor execution model.

- `LOW=1` is compatible with the required physically single-flight actor.
- `MEDIUM` and `HIGH` MUST be rejected before executable lowering. They are
  reserved syntax, not permission to run one actor on multiple native threads.
- A source actor without the annotation has the same physical execution
  semantics as LOW.
- `requestConcurrency` and `messageConcurrency` control outstanding logical
  lifetimes. They do not change the one-physical-turn invariant.
- `ActorGroup` may execute different actors in parallel subject to its own
  aggregate bound and available VM carriers.
- The actor's single protected `state { ... }` schema remains the only
  persistent mutable actor-instance state. A verified logical critical scope
  may span a lowered suspension while retaining exclusion, but any raw/native
  mutable state-access view is revoked before suspension and freshly
  revalidated on resume. Competing state scopes remain blocked until release.
- Java/GraalVM and LLVM may continue recognizing LOW while their staged
  MEDIUM/HIGH syntax fails closed. .NET/WASM/Forge must reject unsupported
  actor execution rather than treating host worker credits as source support.
