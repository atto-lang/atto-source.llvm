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
