# Native actor graph drop order: bounded host contract

## Scope

`ActorOwnedGraphHeap` is a native C++ substrate for future compiler-emitted
AttoLang actor-owned class/struct objects. It is **not** source-to-LLVM
actor object lowering, the complete `await`/cancel unwind pipeline, or a
certified hard-real-time allocator.

## Ordering contract

- Successfully constructed live owners are retired in **reverse construction
  order**, independent of their physical free-slot indices.
- Removing a node detaches its construction-order entry before calling its
  `noexcept` destructor. Reusing that slot appends a **new** order entry;
  an old historical entry must never cause another destructor invocation.
- Heap-wide retirement seals public heap access before destructors and
  scrubs payload and adjacency after deterministic teardown.
- A bounded intrusive doubly linked list uses only existing slot metadata,
  so append/remove take constant time and heap retirement remains
  **O(NodeCapacity)** without tracing or dynamic allocation.
- External borrow/continuation/FFI leases must quiesce before the actual
  retirement proof is consumed; the destructor itself fails closed if
  a lease remains live.

## Source/compiler integration gate

The frontend and LLVM lowering still must bind the ownership CFG
`OwnershipDropPlan` to concrete compiler-emitted initialized/moved flags
and actor-owned region handles. Ordinary class fields currently have
restricted scalar lowering, so source-level destructors for nested
nontrivial class/array/string fields and unwind across guest
`throw`, `raise`, `panic`, `await`, cancellation and actor restart
are **not proven** by this host-only implementation.

An accepted compiler integration must demonstrate:

1. Allocation on the correct live actor-owned heap, with owner/epoch
   identity checked on every emitted access and no guest raw pointer escape.
2. Exactly one destructor for every initialized, still-owned value, in
   reverse source construction order for all exits.
3. A moved owner never receives drop glue, while an initialized field of a
   partially constructed object is still destroyed.
4. All borrows/continuations/FFI leases quiesce before heap storage is
   physically retired.
5. Compiled `.atto`/native AOT fixtures execute, and allocation/GC symbol
   closure is checked on the final linked artifact.

## Evidence

`tests/actor_graph_reverse_drop_test.cpp` checks reverse cleanup after
slot reuse, host C++ exceptional stack unwinding, exactly-once callbacks
and cyclic adjacency. `tests/ownership_drop_plan_test.cpp` separately
checks the CFG-aware per-field drop plan. The existing
`tests/actor_owned_graph_heap_test.cpp` exercises retirement proofs,
leases, cycle purge, generations, and owner isolation.

The independent CI mirror uses exact source Git blob SHA checks,
both GCC/Clang, O1/O2, ASan/UBSan, and a deliberate prepatch failure
oracle. This is **host runtime evidence only**, not whole-program
source-language zero-GC certification.

## Host raw-byte copy isolation

For raw POD nodes, `allocate`, `write`, and `copy_out` reject
nonzero caller spans that overlap any bytes in the graph heap object
(including slots, borrow authority, and link metadata), as well as
uintptr-wrapping spans. This prevents a trusted host adapter from turning
an authenticated actor call into metadata corruption or leaking another
part of the actor's own arena. Zero-length spans remain permitted.

The graph heap still assumes trusted host pointers with valid lifetimes;
this check is not process isolation, cross-actor memory protection, a
replacement for typed compiler-owned object views, or an FFI firewall.
