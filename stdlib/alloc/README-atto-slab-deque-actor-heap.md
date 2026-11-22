# LLVM-native alloc source candidates

These backend-owned `.atto` sources are a starting scalar/word library:
`slab`, `deque`, `actor_heap`. The backend does **not** import Java's
implementations and may independently specialize for native memory and AOT.

**Execution status: NOT EXECUTED.** Current LLVM source-import lowering
does not resolve arbitrary filesystem `atto.alloc/...` module imports.
These files are not installed as executable native library modules; the
`tests/fixtures/alloc/*.atto` positive probes must remain pending until
the import resolver, parser/class lowering and allocator ABI admit them.
No unsupported guest success or native isolation proof is claimed.

Actor heap is a bounded logical `Array<int>` word allocator, not yet the
native u32 actor arena. Physical allocation, owner/turn/carrier fencing,
cancellation, cross-actor non-escape, quiescent retirement and destructor
coverage must remain runtime/compiler-enforced. Logical handle incarnation
and domain integers are not security capabilities.

Contents are an initial scalar policy contract, independently maintained
here. Generalized `T` payload storage requires compiler-checked take/drop,
loan lifetimes and a verified native allocation ABI. Existing LLVM runtime
actor-heap PRs, including #553, must be reconciled before execution claims.
