# Ordered native locals and mutable scalar slots

LLVM lowers typed scalar and sum declarations after calls, guards and loops at
their source position. Immutable record views use the same ordered statement
representation. Initializers execute once when reached; an earlier return skips
them. Stack storage is allocated in the function entry block, so declarations
inside loops do not grow the stack on every iteration.

```ores
fnc initialize(): void { return; }
pub routine main(): int {
  initialize();
  let int total = 0;
  for (let int index = 0; index < 4; index = index + 1) {
    let int increment = index;
    if index == 2; then increment = 10; fi
    total = total + increment;
  }
  return total == 14 ? 0 : 1;
}
```

This supports the router's scalar counters and post-call declarations. It does
not yet admit its mutable, reference-owning `BuildNode` collections.

## Slot mutability and referent capability

Only `let` local slots can be reassigned. A `val` slot remains read-only while
retaining its existing referent capability; a `const` local cannot lend its
value exclusively. Parameter identifiers remain read-only, including
`borrow mut` parameters. An exclusive loan grants referent access, not
permission to replace the parameter binding. Class field mutation keeps its
existing field/receiver checks.

Scalar assignment evaluates the right side before publishing the value. It
requires a visible mutable slot without active loans and checks the exact type,
including a sum's full payload signature. Class owners, arrays and experimental
owned-region values cannot be replaced through scalar assignment. Replacing
those resources requires explicit move, retirement and drop rules.

Names become visible after their initializer has passed checks. Root locals
remain visible through later statements and deferred cleanup. Branch and loop
locals leave scope at the matching edge. Locals cannot shadow live owners, and
loop back-edges must preserve outer ownership and loan state. Definite moves
and restorations remain subject to predecessor-state checks.

Ordered declarations preserve current-main transparent scalar alias and
extension-method resolution. Type-only interfaces/traits remain non-runtime
types; the merged parser must not turn them into scalar storage.

## Lifetime and effect accounting

Root local lifetimes extend through deferred cleanup. A return snapshots its
result before cleanup; the common exit runs defers before ending root-local
lifetimes. Branch temporaries end on their own control-flow edges. Immutable
record views materialize checked root-region descriptors and count as allocation
in realtime analysis, even when their source is a named value.

These declarations do not yet participate in pinned async continuation frames.
Functions combining ordered locals or scalar assignments with the current
`get_ready` await slice reject the source before IR. Likewise, owning arrays
and mutable class owners declared after effects require scoped region/drop
lowering. Their initializers are not hoisted across earlier statements.

## Evidence and remaining Spin work

The Rust parity runner compares source order, skipped initializers, nested
mutable counters, scalar/sum reassignment, parameter restrictions, and deferred
reads with Java. Release and ASan/UBSan runs check the same native programs.
Negative tests cover forward/self references, scope escape, loan/capability
violations, type forgery, owner replacement and unsupported async frames.

The complete router now advances from the post-call declaration at byte 10207
to the mutable `BuildNode` declaration at byte 10442 in the pinned routing
snapshot `1d6ee5bce2dbd795a24e15f69e26a77adfc49808`. Supporting that declaration
requires stable reference-owning collection elements and their borrow/drop ABI;
an immutable snapshot would change how graph mutations behave.

The actual Spin server still requires that ownership work, class-payload sums,
typed HTTP mailboxes, request-scoped storage and await frames. This milestone
does not establish server parity or a latency result.

Current-main integration also retains the direct `get_done` lifecycle boundary
and linear `Future<void>` readiness declarations. The latter remain distinct
from ordered scalar storage. Native continuation storage rejects final-reference
release while a frame is CLAIMED; its claimant must finish execution first.

The regression gates use canonical `rt mut borrow` spelling and complete scalar
alias declarations. Incomplete legacy forward declarations stay rejected.
Immediate `stop()` may cancel startup or abandon queued DATA: execution receipts
must account for every accepted message and verify the processed FIFO prefix,
rather than assuming graceful draining. Exclusive incarnation ownership and
physical reclamation fences remain required before reusing an actor ID.

## Scoped collection owners

Host lowering admits `val Array<T>` and `let mut Array<T>` construction after
statements and inside conditional/index/for-of bodies. Supported elements are
`int`, `String` and the existing immutable record subset. `const` and `let`
without `mut` preserve read-only referent capabilities. Constructor element types
must match exactly; scope names cannot escape or shadow a live owner.

Each static declaration reserves one zeroed cleanup shadow in the invocation's
bounded array region. The array is created only on a reached path and dropped
at the lexical exit or before a structured return. Entry-block handle slots
remain addressable through shared cleanup, so an early return before a later
declaration does not inspect uninitialized storage. Loop back-edges reuse those
slots, release each iteration's resources and clear their failure shadows.
Owning aggregate returns transfer the token and disarm the source before
cleanup. No tracing collector or runtime compilation is involved.

The shared-source gate covers skipped declarations, 2,000 repeated allocations,
nested returns, sibling scopes with different element types, immutable record
iteration and owning aggregate return. ASan instruments the generated guest IR.
Unsupported scoped Wasm owners still reject before emitting IR. Mutable class
owners and general actor/HTTP suspension frames remain separate lowering work;
this support alone does not compile the complete Spin source server.
