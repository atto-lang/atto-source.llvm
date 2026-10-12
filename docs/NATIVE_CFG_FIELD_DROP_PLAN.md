# CFG-aware native per-field exact-once destruction substrate

This is an intentionally **native-host-only** ownership proof increment. The
public Oreslang-to-LLVM compiler currently admits 1–16 `int` class fields;
those fields are trivially destructible, and `llvm.lifetime.end` is not a
destructor. **This change does not enable** class-valued nontrivial fields,
nontrivial per-field `rt take` drop glue, arbitrary constructor failures,
exception unwinding, actor transfers, cancellation or `await`.

`OwnershipDropPlan.hpp` consumes the existing `OwnershipCfg.hpp` fixed-point
analysis to derive reverse-declaration-order cleanup at every reachable CFG
exit. An uncertain joined state such as `Owned|Uninitialized` produces a
runtime-guarded drop, not an unconditional destructor. Native `DropSlot`
holds one concrete per-field ownership flag, an opaque native resource
pointer, and a trusted `noexcept` destructor callback.

Execution preflights **all** flags against the exit's abstract state before
running *any* callback. The native teardown preflight uses only bounded
pairwise comparisons and existing slot-map lookups: it allocates no temporary
sets while cleaning up an owner under memory pressure. If valid, each live owner is marked dropped **before**
its callback executes, and moved, uninitialized or already-dropped fields
receive no callback. A second unconditional cleanup fails closed. Outstanding
borrows and unknown/mismatched field layouts fail admission. Source-ordered
field identities keep reverse cleanup correct independent of string sorting.

Regression `tests/ownership_drop_plan_test.cpp` exercises:

- A diamond where one predecessor initializes a field and the other does not.
- A diamond where one predecessor moves or drops a field.
- Exact-once reverse-order native callbacks on both actual paths.
- Repeated cleanup, invalid flags, live-loan and duplicate-layout rejection.
- Preflight rejection before any destructive callback.
- Duplicate native pointer ownership, stale inactive destructor capabilities,
  invalid bitmasks, duplicate/missing drop places, and extra runtime slots.

This audit preserves the fixed-point ownership CFG analysis and hardens
only its native conditional destruction planner. These callbacks are trusted
`noexcept` capabilities, invoked within one serialized owner/actor cleanup
turn; the planner does not introduce concurrency-safe destructor arbitration.
Run the registered native CTest and its ASan/UBSan configuration against
the exact candidate tree before merging.

**Next compiler integration gates:** allocate concrete per-field initialized
flags when lowering nontrivial source class fields; emit guarded destructor
calls on each normal/early-return edge; prove safe cleanup on partially failed
initialization; integrate typed exceptional and cancellation edges only after
those source-to-native proofs are executable. Primitive i64 class-field takes and the experimental owned-region transfer
already have separate bounded implementations. They are **not** evidence
of nontrivial field ownership/drop parity; retain the compiler's fail-closed
admission for unsupported reference-owning class fields. Track [#96](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/issues/96).
