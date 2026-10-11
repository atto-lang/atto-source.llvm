# SelectPlan / DynamicSelectPlan: native parity gate

**Status:** not implemented in the LLVM backend. This document is the
required behavior contract, not an assertion of native feature support.

The Java reference backend defines:

- **Static select**: reevaluate channel/value case expressions on every
  execution. Any future plan-hoisting optimization must prove expression
  purity, binding stability, actor ownership, and hot-reload invalidation.
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
