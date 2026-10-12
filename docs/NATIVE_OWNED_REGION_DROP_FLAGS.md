# Native experimental owned-i64 conditional drop flags

This document describes **only** the restricted LLVM experimental
`--experimental-owned-int` source subset. It is not an endorsement of a
public `owned` keyword: `owned` remains a valid ordinary identifier in
public Oreslang syntax, and native heap regions remain opt-in.

## Exact-once transfer and destruction

- `const x: owned int = expr` allocates an i64 slot in a bounded native
  `OresOwnedI64Region` (no tracing GC). The compiler also allocates a
  per-local `i1` live flag initialized to true.
- `rt take x` validates an unborrowed, unmoved local owner. LLVM then
  loads the i64, calls `oreslang_owned_i64_region_drop` immediately to
  erase and retire the former native storage, and writes `false` to
  its live flag. The scalar result can be returned/passed/stored.
- Every guarded early return, conditional join and normal fallthrough
  shares the same dynamic flags. Reverse-order exit cleanup branches on
  each live flag, invokes `region_drop` only if true, disarms the flag,
  and closes the region **once** after all slots are retired.
- `OwnershipCfg::DropIfOwned` is applied at every return-only CFG exit
  for a region-owned scalar. The abstract lattice and LLVM runtime flags
  correspond to one another. No pointer or region ownership escapes.
- Double move, use-after-move, live-loan take, divergent move states at
  unrepresentable joins and an `rt borrow` by a deferred cleanup after
  take fail with ownership diagnostics and no emitted IR.
- The **Wasm32 target** continues to reject native owned regions before
  emitting IR. It must not silently link a native allocation ABI.

Native `region_drop` checks provenance, generation-free exact slot
identity, and live state, zeroes its payload and decrements the region's
live count; `region_close` requires zero remaining allocations before
free. The generated flag is what prevents duplicate drops when a moved
slot reaches a distinct exit.

## What this does not implement

This initial mechanism applies to a *primitive i64 moved by value*.
It does not implement nontrivial class destructors, source-level class
members with owning pointers, branch-local class constructors, general
object ABI moves, typed exception unwinding, cancellation, or pinned
continuation ownership. Those require per-field destructor functions,
partial-init rollback, effect-aware CFG edges and frame-state flags,
not simply more `llvm.lifetime.end` markers.

Release gates: real LLVM CMake/CTest, 11 native source-to-Clang
executions under normal and ASan/UBSan instrumentation, no-IR negative
admission tests, Wasm fail-closed validation, and strict shared
Java/LLVM semantics. Tracks #96 and #45.
