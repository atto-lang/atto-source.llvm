# LLVM-local Atto allocation modules

Three public `.atto` source candidates live at:

- `stdlib/alloc/heap.atto`: checked heap reservation, release-policy
  arithmetic and terminal reclamation eligibility;
- `stdlib/alloc/arena.atto`: aligned bump-offset, checkpoint and epoch
  reset planning;
- `stdlib/alloc/region.atto`: owner/incarnation equality, quiescence,
  drop-completion, and non-wrapping region epochs.

The existing `core/arena_limits.atto`, `alloc/arena_lifecycle.atto`
and `alloc/scoped_arena.atto` in this draft remain the deeper LLVM-local
provider/lifetime work. These additional files do **not** replace it.
They do not make physical native allocation available merely by being
present in `stdlib/alloc`. Compiler registry/import wiring and actual
guest execution are still required.

The Atto source does not allocate raw native buffers, issue pointers, mint
physical ownership receipts, free memory or prove zero-GC. The native
`AllocatorProviderV1` and actor region runtime must enforce provenance,
byte quotas, aliases, borrowed scope, zeroing/scrubbing, completion of
compiler-inserted drops, quiescence and physical deallocation. No caller
counter is proof of a live ticket. Fail closed if the provider or compiler
lifetime/drop integration is missing.

`tests/fixtures/alloc-heap-arena-region-policy.atto` is an intended
exit-42 guest fixture, **not executed** here. Required evidence: native
parser/registered import admission, exact-commit guest compilation and
execution, negative forged/stale/cross-owner loan cases, ASan+UBSan, and
AOT cancellation/return/exception paths.
