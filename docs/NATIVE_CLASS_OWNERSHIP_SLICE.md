# Native stack-class ownership: bounded field moves and actor-send admission

LLVM compiles bounded stack classes with 1–16 initialized `int` (`i64`)
fields, explicit same-type local whole-class moves, and checked synchronous
call-scoped shared/mutable borrows. The native CFG proof tracks per-field states
across return-only equality guards. The compiler does **not** yet have a
general-purpose source CFG, nontrivial object destructors, or source-level
suspension; unsupported constructs fail closed before LLVM IR.

## Scope

- `let Pair a = new Pair(); let Pair b = rt take a;` is an admitted
  **local-only, whole-record** transfer of a fully initialized stack object.
  A second transfer, reading moved-from storage, a live loan, type mismatch,
  or a partially moved record must fail without LLVM IR. By-value class
  parameters/returns and actor transfers remain unsupported.
- `rt take a.left` moves a single primitive `i64` **field place**. The
  checker marks that field uninitialized. Reusing it, moving it twice, or
  borrowing/transfering the incomplete class is rejected. Other still-live
  fields remain accessible. `a.left = value;` reinitializes it after the
  right-hand expression is validated, restoring complete-object admission.
- These fields are trivially destructible integers. LLVM lowers a field take
  as a load, coupled with static move-state invalidation. **This is not a
  general-purpose field destructor or ownership-transfer ABI.**
- Synchronous actor `send` payload expressions are checked in original
  send/stop order. An explicit scalar `rt take` cannot be reused in a later
  send or return; raw borrowed references cannot be passed as scalar actor
  payloads. Native scheduler/heap epochs and actor stop checks are retained.
- Ordinary actor refs, class loans and scalar function calls keep existing
  conservative whole-object exclusivity. No split loans, escaping pointers,
  partially initialized nontrivial classes or cross-actor object references.
- Return-only `if (lhs eq rhs) { return expr; } fi` guards remain bounded to
  eight preceding a required final return. A move in a taken return path
  does not affect a distinct fallthrough path. No branch-local object
  constructors or arbitrary `if/else`/loops are admitted.
- The independent field-sensitive `OwnershipCfg.hpp` lattice can compute
  joins, but *runtime conditional drop flags* and exact-once destructor
  glue are not implemented. `llvm.lifetime.end` is a lifetime marker, not
  an object destructor.
- Function-scoped synchronous `defer` (merged LLVM #153) runs LIFO after
  computing the return value; it remains incompatible with guest
  throw/panic/exception unwind and cancellation. Source `await`, typed
  `Future<T>`, pinned guest frames and actor transfers still fail closed.

## Regression coverage

`native_field_and_region_move_admission` generates 1/2/8/16-field positive
read/take/reinitialization cases and rejects double takes, use-after-move,
whole-class borrow/transfer of partial objects, borrowed-field moves and
unsafe native region `owned int` transfers. Every invalid source must emit
**zero LLVM IR** and an ownership diagnostic.

Dedicated native Clang fixtures cover field takes, restoration, guarded return
paths and existing whole-class local moves. The source actor end-to-end harness
executes positive moved-scalar send payloads against the scheduler and rejects
borrows and use-after-move payloads. Full CMake/CTest, Clang ASan/UBSan, and
the strict 11-source Java/LLVM fixture suite remain release gates.

Related: [LLVM ownership #96](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/issues/96),
[suspension-safe ownership #45](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/issues/45).
