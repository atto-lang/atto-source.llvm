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

A source registration may also transfer its arm into a source-owned **BOUND**
state. For selectable send this happens at scheduler submission: after the
handoff, a copied generic token no longer has authority to call the generic
commit/detach APIs. Only the scheduler may commit that BOUND arm at the real
mailbox-credit boundary, or detach it during cancellation/setup failure. This
prevents a coordinator or stale helper from naming the send arm as winner before
the destination can actually accept the message.

The runtime now also has one common, pre-reserved continuation wake per select
generation. It is separate from every source descriptor and retains a
generation-scoped physical loan on the arbiter. A winning source publishes this
wake only after its winner commit; losing sources leave it untouched for the
actual winner. The wake carries winner-arm identity as result metadata and runs
on the selecting actor's authenticated continuation lane. Arbiter rearm requires
both zero active arm tokens **and zero physical select loans**. Wake identity is
cleared before the physical-loan count reaches zero, so a later generation
cannot alias a RESERVED/READY/EXECUTING callback from the previous generation.

This is intentionally **not** implemented by wrapping a normal READY
`send_async` Future after the fact. Ordinary READY already means irrevocable
mailbox admission, which is too late for a losing select arm to roll back.

Static FAIR state is also scoped to the compiler source site, not to the
process/VM group. The current two-send source scaffold keeps a fixed 64-entry
table keyed by the compiler source token; each site owns its own monotonic
ticket and normalizes that ticket by the arm count. Different static select
sites therefore cannot perturb each other's first/next arm order. The table is
bounded and site/ticket exhaustion fails closed rather than allocating or
wrapping. This matches the Java static-ticket contract and must eventually be
extended with explicit actor/site identity when native select is admitted
inside actor methods.

Additional stale-generation hardening is normative for this substrate:

- the generation-scoped `arm_count` is atomic metadata and is published before
  the OPEN release-store; registration acquires OPEN before reading the bound,
  then rechecks generation/bound after taking its active-token fence;
- seal requires the caller's exact selection generation. A delayed cancellation
  or finalizer from generation N cannot detach registrations in N+1;
- async scheduler callbacks observe the winner through a generation-fenced
  query using the generation carried by their descriptor/common-wake capability.
  A stale completion cannot attribute a later generation's winner to itself.

Current non-claims remain important:
- the source parser/type checker still rejects native static/dynamic select;
- the common wake is runtime substrate only; compiler-generated select
  continuation capture/result storage is not yet lowered from `.ores`;
- Channel/Future/stream arms are not yet integrated with this arbiter;
- FAIR/priority/random source policies are not yet implemented;
- object mailbox payloads remain outside the current scalar source slice.

The next vertical step is compiler-owned select continuation/result storage
using this common generation wake, followed by one static two-arm compiled
`.ores` fixture. Until that source lowering is complete, existing fail-closed
select fixtures must remain unchanged.

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
