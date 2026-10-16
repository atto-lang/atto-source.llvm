# Native Oreslang primitive field loans (bounded source slice)

The LLVM backend accepts call-scoped projections of **public, primitive i64
class fields**. These are real in-place pointers into a stack-allocated,
ownership-checked native class record; they are not temporary copied values.

```ores
define class Player as
  pub let int health = 20;
  pub let int stamina = 22;
end

fnc inspect(borrow int value): int { return value; }
pub fnc main(): int {
  let Player p = new Player();
  return inspect(rt borrow p.health) + inspect(rt borrow p.stamina);
}
```

## Deliberately conservative alias model

A projected `rt borrow p.health` is tracked as a **shared whole-owner loan
of `p`**, and `rt borrow mut p.health` as an **exclusive whole-owner
loan**. Its lifetime starts during left-to-right argument evaluation and ends
when the synchronous callee returns. Even a projected mutable borrow of
`p.health` conflicts with a simultaneous shared borrow of `p.stamina`.
This loses some valid programs but prevents disjoint-field alias assumptions
from bypassing the currently whole-owner source borrow checker.

A projection is allowed only if the class instance, field and all containing
owner state are live; the field is public and primitive; and no unproven
partial move, outstanding conflicting loan, or nontrivial owned `Array<int>`
class field exists. A mutable projection additionally requires a mutable
owner or a mutable borrowed parameter; a shared borrowed parameter can never
be strengthened into a mutable loan.

Unsupported direct/nested/dynamic pointers, temporary class fields, private
fields, moved fields, array/owned-handle fields and wrong borrowed parameter
types reject before LLVM IR emission. No projected pointer crosses an actor
send, await, select, cancellation, exception, or async continuation. This
slice neither adds source methods for scalar write-through nor supports
simultaneous independent mutable field loans. Those require a provenance-aware
loan tree and CFG ownership/lifetime proof, not merely native GEP addressing.

## Verification and merge gates

`tests/native_projected_field_borrow_matrix.py` executes four LLVM IR ->
Clang guest binaries (all must exit 42) and six negative sources (all must
reject with zero IR), including immutable-owner exclusive loans, partial
moves, private fields and cross-field alias conflicts. Run both under normal
and optimized Python, plus ASan/UBSan on every positive compiled guest.

The funded [exact-source compiler verification
run](https://github.com/zed-pkg-test/java-app/actions/runs/38841936864)
checked 26 immutable Git blobs, compiled the production LLVM frontend,
executed the established multi-field layout and alias matrix, and passed all
new tests and four sanitized guest executions. This is **focused test
evidence**: the full repository CTest matrix and broad Java/Graal differential
parity are separate merge gates.
