# SelectPlan / DynamicSelectPlan: native parity gate

**Status:** full typed/static select is not implemented in the LLVM backend.
The native frontend now admits only the first procedural source slice:
blocking `do select` with exactly one `default` arm, using either
`{ ... }` or `over ... end`. Every channel/Future arm, value-producing
`select`, `nb select`, `do nb select`, policy, plan and dynamic-plan form
remains fail-closed. The rest of this document is the required behavior
contract, not an assertion of broader native feature support.

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

This arbiter is **not** by itself source-level select support. Generic
`Channel<T>` registration, typed arm bindings, blocking suspension,
continuation scheduling and policy lowering (`first`/`fair`/`random`) are
still required for real multi-arm source selection. A later runtime slice now
also provides one-shot i64 Future readiness and actor-generation fencing, but
those capabilities are not yet wired into source arms.

The only admitted frontend form is the degenerate procedural case whose sole
`default` arm is immediately eligible: blocking `do select` with exactly one
`default`, in braces or `over ... end`. It lowers as a real structured
statement so lexical scope, ownership checking, mutation and callable return
reuse the normal CFG/cleanup machinery. Any read/write/await arm or any other
select mode continues to fail closed.

## Selectable bounded i64 channel substrate

A stacked runtime slice adds `NativeSelectableI64Channel<Capacity, MaxWaiters>`
for the first native scalar select profile. It is deliberately narrower than
source `Channel<T>` and exists to prove the channel/select transaction before
frontend lowering.

The runtime contract is:

- fixed-capacity i64 buffering;
- revocable readable/writable subscriptions with generation-checked identities;
- readiness callbacks never run while the channel mutex is held;
- callbacks are advisory/spurious and must recheck through the arbiter;
- a read removes a value only inside `NativeSelectArbiter::try_win_if`;
- a write publishes a value only inside `NativeSelectArbiter::try_win_if`;
- a losing read therefore cannot consume, and a losing write cannot publish;
- state transitions capture opposite-direction notifications before mutation,
  then dispatch them after select/channel locks are released;
- stale captured notifications are harmless because commit validates the
  subscription identity again;
- notification callback failures are retained for diagnostics without rolling
  back a committed channel operation.

Channel close/EOF is now part of this slice: close wakes pending readers and
writers, buffered values drain before EOF, an empty closed channel commits EOF,
and a selected write against a closed channel commits a terminal closed outcome
without publication. The stacked runtime also supplies revocable one-shot i64
Future readiness and an actor-generation lease that fences the full bounded
select commit against stale continuations.

Unbounded/dynamic channels, typed values beyond native i64, fairness/random
policy, suspension and actor continuation delivery remain outside this slice.
The frontend therefore still rejects all channel/Future select arms and all
nonblocking/value-producing forms; only the default-only blocking procedural
`do select` slice described above is admitted.

