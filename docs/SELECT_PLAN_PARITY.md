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

## Native static-select arbitration substrate

The native runtime now has a host-side `NativeSelectArbiter<N>` substrate for the
linearization point needed by future static-select lowering. It proves a narrow
runtime contract only:

- exactly one registered case can win;
- armed losing registrations are revoked exactly once;
- cancellation revokes every armed registration exactly once;
- a completion/cancellation that wins before an external registration returns
  its revoker is remembered, and `arm()` immediately revokes that late loser;
- loser revocation callbacks run outside the arbiter mutex and all revokers are
  drained even if one callback throws;
- `TaskCancellation` can own the arbiter through a lifetime-safe cancellation
  handle.

This is **not** source-level select support. `Channel<T>`/Future registration,
typed arm bindings, ownership/borrow checking, blocking suspension,
continuation scheduling, channel-close semantics, policy lowering
(`first`/`fair`/`random`), and actor generation fencing are still required.
Until those layers are wired and tested, source `select`, including both
`{ ... }` and `over ... end`, must continue to fail closed at the native
frontend boundary.


## Main-stack runtime integration

The native runtime prerequisites are now carried on a clean branch rooted at the
live main tree rather than relying on feature branches whose parent PRs had
already merged.

The integrated runtime contract includes:

- bounded selectable signed-i64 channel readiness;
- channel close/EOF with buffered drain-before-EOF and terminal closed writes;
- immutable/non-consuming signed-i64 Future readiness with value/failure/cancel;
- exactly-one select arbitration with loser revocation;
- actor-generation continuation tickets with ABA-safe identities;
- a claimed continuation retains a physical execution lease through callback
  completion;
- actor generation advance fails while any claimed continuation is still
  executing;
- terminal close cancels pending delivery but physical retirement is not
  reported complete until every claimed continuation has released its lease.

This closes the gap where a scheduler could consume a logical continuation
ticket and then advance the actor generation before the continuation physically
returned.

Source-level static select remains fail-closed. The next compiler slice must
bind typed read/write/Future arms to these runtime transactions, store the
winner result in compiler-owned continuation storage, revoke borrows before
suspension, and reacquire/revalidate actor-local state on resume. Parser
admission must not be enabled before those ownership and suspension checks are
executable.
