# LLVM native frontend admission note

The LLVM source frontend currently supports a bounded scalar native subset.
This branch recognizes the bare `#identifier` token, but **rejects** it
with `native Symbol literal requires tagged-value` rather than silently
coercing a Symbol into an integer. Likewise, `Scheduler` and `Region`
guest values fail with a dedicated admission diagnostic; the existing
ActorGroup source boundary remains fail-closed. CTest fixtures pin all
three negatives to prevent accidental unsafe native IR emission.

**This is lexical recognition and explicit rejection, not usable source
scheduler, Region, ActorGroup compute, or native lambda support.**

The native frontend also explicitly rejects the product literal surfaces that
Java/Graal now distinguishes semantically:

```ores
record { x: 3, y: 4 }                         // structural record
infer struct { x: 3, y: 4 }                   // concrete inferred struct
struct { x: int, y: int } { x: 3, y: 4 }      // explicit anonymous struct
```

These forms must not accidentally lower as scalar identifiers or as one common
anonymous-object type. Native admission stays closed until LLVM has distinct
record-vs-struct type identity, exact struct-shape checking, ownership/drop
plans, ABI/layout rules, and cross-backend conformance. Dedicated CTest
fixtures require rejection before LLVM IR publication.

Computed `[]` access is also explicitly rejected in the native scalar frontend.
Java/Graal now gives that syntax receiver-directed semantics (integer tuple/list
positions versus string record/struct/class fields), exact-vs-union result typing,
class visibility rules, mutation authority, and definite-initialization checks.
Native LLVM must not accept `value[index]` until it can preserve those same
type/ownership guarantees. The `source-computed-index-unsupported.ores` fixture
pins this fail-closed boundary before IR publication.

---

# Scheduler / ActorGroup / Region compute syntax — cross-backend contract (2026-10-19)

**Status:** staged language contract. This document does **not** establish executable
`Scheduler.new`, `Region<T>.new`, or `ActorGroup.compute` support. A parser
accepting a token is not a proof of guest lowering or safe runtime execution.

## Canonical proposed source

```ores
let scheduler = Scheduler.new(
    workers: #auto,
    affinity: #soft,
    topology: #numa_aware,
    work_stealing: #hierarchical,
    task_migration: #safe_points
);

let group = ActorGroup.new(
    scheduler: scheduler,
    sharding: #cpu_local,
    batch_dispatch: true
);

let motion = Region<Motion>.new(
    layouts: [#aos, #soa, #aosoa],
    authority: #soa,
    tile_size: 32,
    placement: #numa_local
);

await group.compute(
    region: motion,
    layout: #aosoa,
    reads: [Motion.vx, Motion.vy],
    writes: [Motion.x, Motion.y],
    kernel: fnc (chunk) {
        chunk.x += chunk.vx * dt;
        chunk.y += chunk.vy * dt;
    }
);
```

The above is a **target fixture, not presently executable source**.
In particular, Java does not yet bind named value arguments to a
declaration; the `+=` operator lacks a verified single-evaluation compound
assignment IR; the scheduler/region compute APIs and field selectors are not
lowered. LLVM native source is a deliberately restricted compiler subset.

## Syntax contracts

1. **Anonymous functions:** `fnc (chunk) { body }` is exactly the same
   lexical, capture, ownership, lifetime, callable-type and result semantics
   as `|chunk| -> { body }`. Both lower to a single `LambdaExpr`; `nlex`
   has the same capture-barrier behavior. Unlike expression-bodied pipe
   lambdas, anonymous `fnc` requires a braced statement body. Typed params
   (`fnc (MotionChunk chunk) { ... }`) are accepted syntactically.
   No actor is created merely by using `fnc`.
2. **Inline collections:** `[a,b,c]` and `arr[a,b,c]` are existing value
   list literals, preserving source order. Here they require homogeneous
   symbol or field-selector element types determined by the expected API.
   They are **not** tuple types, argument spreads, or three mutable aliases.
3. **Named *value* arguments:** use `name: expression`; compile-time
   generic metadata continues to use `Type<name=value>`. An invocation
   must resolve against a statically known signature. Reject duplicate,
   unknown, missing and positional-after-named arguments; evaluate each
   expression once in lexical order, bind to declared parameter slots
   afterward. Never implement labels as a runtime map or reorder evaluation
   to match declaration order. In particular, the user's original bare trailing
   `fnc (chunk)` after named arguments is invalid; the canonical call uses
   `kernel: fnc (chunk) { ... }`, not a positional-after-named exception.
   Current Java work in
   `docs/NAMED_ARGUMENTS_CONTRACT.md` and drafts #1335/#1397/#1408
   does **not** yet make general guest named calls executable.
4. **Symbols:** `#auto`, `#soft`, `#numa_aware`, `#hierarchical`,
   `#safe_points`, `#cpu_local`, `#aos`, `#soa`, `#aosoa`, and
   `#numa_local` are interned symbol *values*, not identifiers, strings,
   keywords or automatically trusted enums. Option APIs must check accepted
   symbol cases statically when possible and validate at runtime otherwise.
5. **Compound updates:** `chunk.x += rhs` must evaluate the target's base
   and index only once, enforce unique field-write permission and preserve
   checked arithmetic. Do **not** desugar arbitrary lvalues to
   `chunk.x = chunk.x + rhs` without proving no repeated evaluation,
   borrow conflict, or observably different ordering.
6. **Type member selectors:** `Motion.vx` denotes a compiler-validated
   field selector for `Region<Motion>` when used inside `reads`/`writes`.
   The compiler must reject unknown fields, duplicates, aliasing conflicts,
   or selectors from a different nominal type. These are never general
   reflection pointers or unrestricted access to another actor's heap.
   Current Java `parsePostfix` does not yet parse `Region<Motion>.new(...)`:
   unlike `Region.new<Motion>(...)`, its generic type arguments precede
   a member access. This requires a dedicated type-member AST and binding
   path; do not silently attach type specialization to an unrelated method.

## Execution / concurrency invariants

- `ActorGroup` remains a **membership domain**, not a supervisor,
  task owner, or authority to access members' actor-local heaps. Group
  compute must use explicitly owned/authorized Region storage, not implicit
  unrestricted mutable actor state. Lifecycle ownership remains with the
  actor's supervisor; no group operation may launder private pointers.
- One authoritative writable layout per region epoch. The `layouts` list
  can request several stored representations, but any non-authoritative
  AoS/SoA/AoSoA copy is a versioned projection reconciled before access.
  `authority: #soa` chooses initial truth; `layout: #aosoa` chooses
  dispatch representation and needs an explicit permitted writeback/authority
  transition. No simultaneous unsynchronized writers or hidden triple
  updates; memory amplification must be budgeted.
- Chunk jobs receive disjoint, generation-checked slices and field-sensitive
  read/write permits. Borrowed chunk views never escape the callback, suspend
  across `await`, or enter an actor mailbox. `group.compute` awaits after
  **all** task writes/cleanup are physically complete (also on cancellation).
  A conflicting overlapping writable region must be rejected before dispatch.
- Closure captures such as `dt` must satisfy checked Send/Sync/ownership
  rules; detached or cross-worker execution cannot capture actor-private,
  unpinned mutable, revoked, or non-transferable borrows.
- `workers: #auto` is a bounded policy resolved from available schedulable
  carriers; `affinity: #soft` and `#numa_local` are hints, not guaranteed
  pthread pinning or a correctness precondition. Fail explicitly or degrade
  predictably on targets with no NUMA topology.
- `#hierarchical` work stealing prefers local queues, then NUMA nodes,
  then global work subject to fairness and backpressure. `#safe_points`
  permits *logical task* migration between workers only after quiescent,
  compiler-verified suspend/cooperation points: never migrate a live
  pinned/native/borrowed stack or change OS pthread affinity per actor.

## Promotion gates

1. Java: committed anonymous `fnc` alias parser and AST-equivalence tests
   must pass on an exact-source JUnit/Maven run. Grammar and formatter must
   agree on canonicalization (formatter may choose pipe spelling).
2. Java and LLVM: first-class named `CallArgument` IR; declaration-driven
   binder and evaluated-in-source-order ABI plan; positive/negative fixtures.
3. Both frontends: single-evaluation compound-assign IR and type-checked
   `Region<Motion>` member selectors.
4. Typed core/alloc/std library APIs for `Scheduler`, `Region`,
   `ActorGroup.compute`; preserve freestanding core vs optional OS policy.
5. Guest-to-scheduler executable slice (disjoint scalar chunks first),
   exact-head CI, sanitizer stress / races, NUMA-independent fallback tests
   and cancellation/cleanup conformance across Java min-GC and LLVM zero-GC.

**No current PR should claim the complete example runs on either backend.**
