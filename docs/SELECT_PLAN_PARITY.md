# SelectPlan / DynamicSelectPlan: native parity gate

**Status:** not implemented in the LLVM backend. This document is the
required behavior contract, not an assertion of native feature support.

The Java reference backend defines:

- **Static select**: evaluate channel/value expressions on every execution
  except when the compiler proves that a specific source site is safe to
  hoist. Java PR #526 now proposes lexical-loop-only cached descriptors for
  read-only select arms bound to non-shadowed immutable val/const channels.
  Dynamic operands, WRITE arms, mutable bindings, and shadowed bindings
  continue to be evaluated each iteration; LLVM remains fail-closed.
- **Immutable SelectPlan**: reuse an immutable case descriptor list and its
  fairness cursor. Each operation must have fresh winner/cancellation state.
- **DynamicSelectPlan**: keyed insertion-ordered cases with atomic put/remove
  mutation and immutable, versioned snapshots. Replacement retains key
  position. Pending operations remain bound to the old snapshot. Results
  with a stable case key must also identify the snapshot version.
- **Admission ordering and lock safety**: sample FAIR case rotation when
  the operation enters the serialized native selection arbitration point,
  not before it enters that coordinator. Updates from a selected case must
  not acquire a mutable-plan monitor while holding a channel coordinator:
  this admits an application-visible inverted lock ordering and deadlock.
  Use per-generation atomic fairness state or an equivalent acyclic protocol.
- **Generation ownership**: winning an operation from a superseded
  DynamicSelectPlan snapshot must not rotate the FAIR cursor of a newer
  snapshot; default selection consumes no round-robin credit. All updates
  must reject version overflow instead of silently reusing old generations.
- **Typed source boundary**: constructing a plan from a non-SelectCase
  list/map and selecting from unrelated values must fail at type-check time,
  not register an invalid native waiter.
- **Fair** policy rotates among ready cases; explicit priority is ordered;
  random policy is opt-in. Close, rendezvous, cancellation, losing-arm
  detachment and default dispatch must share atomic arbitration.
- **Actor isolation**: plans and channels are actor-domain-local runtime
  capabilities; never serialize/share them through mailboxes.
- **Allocation/performance**: reusing case descriptions is not reusing a live
  channel waiter. Do not claim zero allocations or a speedup until native
  benchmarks demonstrate it.

## Borrow-checker parity requirements

The Java source checker now treats `SelectCase` and `SelectSet` as
**move-only** descriptors, independently of the Copy classification of
`Channel<T>` handles. Native codegen must preserve these rules:

- `SelectCase.read(channel)` must not retain a lexical `&Channel<T>`
  reference. It may retain a copied actor-domain-local channel handle.
- `SelectCase.write(channel, value)` requires a Copy payload because the
  descriptor may execute repeatedly. Reject borrowed, guard-bearing, and
  move-only payloads. One-shot `writech` or static select remains the path
  for move-only transfers until a linear one-shot case is designed.
- `SelectSet.new(cases)` takes ownership of its case collection, not a
  borrowed reference into a stack frame. The set is move-only, while
  `select from set` borrows the existing descriptor without consuming it.
- A `DynamicSelectPlan` mutation cannot install an escaping borrow into
  a versioned snapshot. A pending snapshot owns its captured operands
  until one winner or cancellation releases registrations.
- Plan/case/set capabilities cannot be copied via Option/Result, structs,
  tuple or array wrappers, stored in actor-crossing messages, or implicitly
  shared between tenants/actor domains.
- Compiler hoisting of a static select must prove that per-iteration
  case expression evaluation can be removed without changing ownership,
  side effects, actor affinity or hot-reload generation.
- Native negative fixtures must reject double moves, borrowed channels,
  borrowed case collections, reused move-only write payloads, and
  select-plan mailbox transport. Positive fixtures must permit multiple
  selects on the same retained set with Copy write payloads.

Java reference validation: [45 passing GraalVM compiler/runtime tests, plus three repeated dynamic-plan suites](https://github.com/athlet-o/athleto-e2e/actions/runs/38080964024) for the integrated PR #526. The concurrent plan-mutation stress and single-use SelectResult payload extraction checks are included. This does not verify LLVM native behavior.

Native fail-closed CI: [26/26 LLVM CTest contracts passed](https://github.com/athlet-o/athleto-e2e/actions/runs/38081134475), including explicit rejection of SelectPlan constructors and persistent SelectCase writes without emitted LLVM IR. Those tests verify *rejection*, not native borrow-checker implementation.

These requirements are not implemented by this documentation change.
The current LLVM bootstrap must keep rejecting unsupported select syntax
rather than emitting IR with unsound ownership assumptions.

## October 26 — first scheduler-native selectable send substrate

The native runtime now has a **host/runtime-only** first slice for a mailbox
send candidate. This does not enable any guest `select` syntax yet.

The slice deliberately reuses the scheduler's existing bounded
`send_async` descriptor pool and target-local FIFO credit arbitration.
A generation-fenced `OresNativeSelectArbiter` owns one common winner cell;
the send descriptor retains only one opaque select token. When target mailbox
credit becomes available, the scheduler:

1. preflights execution-id and worker capacity while holding its existing
   admission mutex;
2. atomically commits the shared select winner;
3. enqueues the mailbox payload **only if that send arm won**.

A losing or sealed send arm becomes CANCELLED without message publication.
STOP or a **post-registration** send failure is a terminal select event: it may
win the common selection and preserve the exact failure, or becomes CANCELLED
if another arm already won. In contrast, descriptor/waiter quota exhaustion
while registering the arm is a **select setup failure**, not a ready arm; the
arm detaches without claiming the winner and the coordinator must clean up the
partial selection. If another arm commits while that setup failure is being
classified, the send arm reports CANCELLED rather than surfacing a stale
RESOURCE_EXHAUSTED result over an already-completed selection.

Registration itself is part of the generation lifetime fence: an arm enters
`active_tokens` while still REGISTERING, before rechecking OPEN/generation.
Therefore seal -> rearm cannot reset arm state underneath an in-flight
registration. Selection generations are strictly monotonic and never reusable;
same/older generations and terminal generation exhaustion fail closed. Stale
selection/arm generations fail closed. The select arbiter is fixed-capacity and
performs no allocation or guest callback during commit.

This is intentionally **not** implemented by wrapping a normal READY
`send_async` Future after the fact. Ordinary READY already means irrevocable
mailbox admission, which is too late for a losing select arm to roll back.

Current non-claims remain important:
- the source parser/type checker still rejects native static/dynamic select;
- there is not yet a common source continuation wake bound to a select
  generation;
- Channel/Future/stream arms are not yet integrated with this arbiter;
- FAIR/priority/random source policies are not yet implemented;
- object mailbox payloads remain outside the current scalar source slice.

The next vertical step is a common generation-fenced select continuation that
all registered sources can notify after arbitration, followed by one static
compiled `.ores` fixture. Until then, existing fail-closed source fixtures
must remain unchanged.

## October 26 — one common select-generation continuation wake

The next runtime layer reserves **one** producer continuation wake for an
entire select generation. It reuses the scheduler's existing preallocated
eight-credit wake escrow; there is no per-arm guest callback queue.

The scheduler mints the nonzero selection generation and reserves one generic
wake bound to the producer continuation. The coordinator then:

1. registers **all** arm tokens in the fixed-capacity arbiter;
2. attaches every token to that exact common wake identity;
3. only then submits source-specific operations.

An arm token carries numeric wake coordinates plus the arbiter pointer and
selection generation. The underlying `WakeCredit` is independently tagged
with that exact arbiter pointer/generation. This extra tag is required because
ordinary generic wakes also use `descriptor_slot == UINT64_MAX`; descriptor
coordinates alone are not proof of select authority.

When an async mailbox send wins, the scheduler publishes the common wake
automatically after the send outcome and mailbox admission are committed.
A pending send that wins with STOP/resource failure publishes the same wake as
FAILED. A losing send publishes nothing. For an **immediate synchronous
terminal** result (for example, target already stopped), the send operation
commits the select winner but does not publish the wake itself: the coordinator
must first store the exact result, then publish the common wake, so the resumed
continuation cannot race uninitialized result state. Non-send sources follow
the same record-result-before-publish rule.

The common wake's `completion_code` is `arm_index + 1`; it identifies the
winner but is not a guest value or error payload. Source-specific result/error
storage remains in the compiler/runtime select frame.

Generation reuse is physically fenced, not merely logically fenced. A select
arbiter may rearm only after:

- the prior generation is terminal;
- all prior arm tokens are retired; **and**
- the prior common wake has fully left RESERVED/READY/EXECUTING and the wake
  callback has returned.

The scheduler tags and scans the exact arbiter/generation to prove that last
condition. Old tokens cannot publish a new generation's wake, and ordinary
generic wakes cannot alias a select wake.

This still does **not** enable guest source `select`. The next compiler slice
must allocate one bounded select continuation frame, lower a tiny static arm
set to this runtime protocol, clean up all loser registrations on partial setup
failure, and prove one real `.ores` program through LLVM and native execution.
Existing unsupported source-select fixtures remain fail-closed until that
vertical proof exists.

## October 26 — first executable source try-select slice

LLVM now has one deliberately closed source/compiler vertical slice over the
same scheduler-native select authority:

```ores
try select {
  when worker.send_async(1): -> {
    chosen = 43;
  }
  default -> {
    chosen = 42;
  }
}
```

This form is nonblocking and accepts exactly one `ActorRef.send_async(i64)`
operation plus one explicit default arm in trusted zero-argument `main`.
Both arms are synchronous scalar assignments in this first slice. The compiler
builds an explicit `NativeTrySelectSite`, type/ownership checks both branches,
and lowers one checked runtime call plus real LLVM winner/default CFG.

The runtime uses the existing scheduler async-send descriptor and the same
`OresNativeSelectArbiter`; there is no source-only mailbox queue or shadow
credit state. If mailbox credit is immediately admissible, the send arm must
win before message publication. If the send remains pending, default wins,
the losing descriptor is cancelled/released, and no payload may later appear.
Terminal send failure remains an operation event rather than silently falling
through to default.

Selection generations never wrap or recycle. Even this stack-local try-select
fails closed at generation exhaustion so later suspension/general-select work
cannot inherit an ABA exception.

This does **not** implement blocking `select`, `nb select`, Channel
read/write, Future completion arms, Observable/Stream arms, dynamic
`select from`, arbitrary branch bodies, or source-level use of the common
generation wake. Those remain fail-closed until their compiler/runtime
continuation and ownership contracts are executable.

## Native implementation acceptance requirements

1. Typed channel cases (read/write/default), copy/ownership rules, and
   source AST/HIR support for both `select { ... }` and `select from plan`.
2. Native actor-owned select registration with atomic exactly-one-winner,
   cancellation, close, wake-up, and stale-generation protection.
3. Immutable-plan and mutable-plan snapshot storage with versioned keyed
   results; race-safe add/remove/rebind and bounded allocation behavior.
4. Differential conformance against Java: fair/priority/random ordering,
   old/new snapshot isolation, default, two-sided rendezvous, pending
   removal, cancellation, channel close, actor teardown and hot reload.
5. Native memory/race sanitizer coverage and measured benchmarks versus
   ordinary static and dynamic selects, for constant and mutating case sets.

The current bootstrap parser supports only integer-returning functions.
The negative `unsupported-static-select` and
`unsupported-dynamic-select-plan` fixtures require **nonzero exit with
no LLVM IR emitted**, preventing premature claims of parity. Do not loosen
these tests until the relevant native feature and its conformance suite are
implemented.


## October 20 — loop-local hoisting and static FAIR tickets (proposed Java #526)

The revised Java draft proposes a restricted interpreter optimization:
cache `SelectSet` descriptors on each executing loop's `Env` for
READ/DEFAULT-only static select sites where all channel operands are direct
immutable non-shadowed lexical bindings. The cache is *not* attached to
shared source AST nodes and cannot survive the loop invocation, actor
generation or hot reload. No user-visible static/async select effect should
change.

Native codegen **must not** copy a plan between actors or across a dynamic
state/ownership boundary. It must either reconstruct the cases on each
iteration or statically prove the same read-only/stable-binding predicate.

The source static selection cursor has a crucial compatibility contract:
each invocation consumes the owning actor's original static FAIR ticket,
including immediate misses/default selections, **independently** of which
case wins. Java exposes a compiler-only `SelectSet.selectStaticAsync(policy,
ticket)` / `tryStaticSelect(policy, ticket)` path that snapshots this ticket
on an individual registration under arbitration. Cached descriptors must
not replace that with their internal win-advancing FAIR cursor; doing so
would change source-level selection ordering, especially with cancellation
and concurrent pending selections. Old pending registrations cannot retarget
a subsequent source ticket or new dynamic-plan generation.

### Required native differential fixtures before feature activation

- Two always-ready channels, repeated static FAIR selection with first,
  default, cancellation, and try-miss in the same loop.
- Nested loops and separate actor instances compiled from the same AST.
- Shadowing immutable channel names inside a per-iteration body versus
  immutable bindings outside the loop.
- Mutable channel binding reassignments, side-effectful channel factories,
  persistent WRITE payloads and scope-finalization exits: **must not hoist**.
- Dynamic plan add/remove/rebind with old pending winner; stable result keys
  and generation-isolated FAIR state.
- Actor stop, module hot reload, timeout cancellation and Future completion
  races, with no guest callbacks under channel/coordinator locks.
- Allocation and throughput benchmark parity under JDK25/GraalVM and native
  ASAN/TSAN builds; never claim zero allocation merely for descriptor reuse.

### Mixed readiness

Future, deadline, cancellation-token and default readiness from Java PR #433
are not yet integrated with the Java optimized plan in PR #526 and are not
native features. The native implementation must allocate fresh per-invocation
subscriptions/timers, reset relative deadlines, arbitrate a single winner,
revoke loser subscriptions and reject stale callback generations before
emitting any LLVM IR for these source forms. Preserve typed Option/unit
payloads and checked move-only ownership; fail closed otherwise.

**Evidence status:** this is a forward contract, not a native
implementation or new native test result. No current-head JDK25/GraalVM or
native differential evidence accompanies this documentation update.
