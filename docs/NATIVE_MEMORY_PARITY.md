# Native actor memory parity: first executable slice

Status: LLVM source-ownership and reachability fixes plus native actor runtime integration; not whole-language certification.

## Separate promises

- **Full guest AOT:** every reachable guest function is native before launch;
  no interpreter, guest JIT, source parsing or compilation fallback at runtime.
  A native executable containing an interpreter does not meet this promise.
- **Zero guest GC:** guest state and reachable runtime services use deterministic
  ownership/arenas rather than tracing collection. Native Image and .NET Native
  AOT alone do not imply this. Allocation-free turns are a narrower property.
- **Borrow checking:** reject incompatible loans, moves, drops and escapes
  statically. Runtime loan validation is defense in depth, not a substitute.
- **Actor-owned storage:** each actor incarnation owns its mutable storage;
  carrier migration must not change ownership. Domain-checked handles do not
  establish an OS address-space or untrusted-code sandbox boundary.

## Audited baseline (2026-10-20)

The following is a source inspection baseline, not an executed certification
matrix. Open feature branches may provide additional behavior.

| Backend | Inspected main | Current boundary relevant to this work |
| --- | --- | --- |
| Java | a80dedb5776b8762eccfb753eda30543075f19bd | Host AOT mode permits interpreted guest execution; managed generational graph helper explicitly does not establish zero GC. Existing static ownership checker must remain authoritative. |
| LLVM | 5cd61d63f97cc3963fae3afee14fb1043d0659e3 | Native guest subset and bounded C actor arena; host-only arena tests do not establish guest aggregate or async lowering. |
| Forge | eadfdc44b9dc51610b09103ed28879403b768abf | Scalar native emission and byte mailbox; this change adds bounded arenas and an explicitly scheduled host scalar-actor runtime. Forge guest actor parsing/lowering remains unsupported. |
| .NET | 74204676ed620b6aceea22574953c34de8583823 | Native AOT subset; managed runtime GC and mailbox are not per-actor private heaps. |
| WASM | 641c3cc08b44376c5a4ab30882482c1efc0749f8 | LLVM compiler bridge and separate AOT-host tooling; worker/linear-memory isolation is not guest actor/borrow-checker parity. |
| Rust | 01b7d9ef33ed425d6f2f59dc37eaf96ed7a434e3 | Narrow Cranelift native-object subset; no claim of guest actor ownership support. |
| JS | 684a9bd47fcd2ef9cebf7dcaf6e319d4c8011542 | ESM generation; source compilation ahead of deployment does not remove host GC or guarantee native AOT. |
| HDL | 1ee16c1d5b07eecb8e71d516e4fd24624812fd5e | Bounded hardware prototype; native actor-heap requirements are not a hardware implementation claim. |

Sources: each backend's pinned README, LLVM `docs/FULL_AOT.md`,
Java `ExecutionProfile.java` and `GenerationalGraphArena.java`,
and LLVM/Forge runtime and compiler sources. No other backend is promoted by
the tests in this change.

## Implemented native memory slice

LLVM and Forge now use byte-identical C arena source/header implementations.
Forge exposes `oresforge_actor_arena` and `oresforge_scalar_actor` libraries.
The latter binds the compiler scalar-heap ABI to one private heap and bounded
preallocated FIFO per host actor. Forge guest actor syntax is not yet lowered.

- Preallocate bounded persistent slots, scratch bytes and loan metadata at open.
- Enforce domain, actor identity/incarnation, current turn and physical carrier.
- Permit multiple shared loans OR one exclusive mutable loan per whole slot.
- Block owner reads while exclusively loaned, and owner writes/drop while loaned.
- Read/write through loans only after validating live issuance and current turn.
- Keep loans durable across turns, while scratch is zeroed on leave.
- Revoke I/O during retirement; permit release and turn unwind, and delay destroy
  until the active turn and all loans drain.
- Reject stale/replayed loans after table reuse and foreign-arena handles.

Loan tokens are trusted-host values. Do not expose these C structs to untrusted
guest code or use numeric fields as a cryptographic authority. All holders and
callbacks must be fenced before freeing the arena. This API is not freestanding
core: initialization/destruction use the host allocator and abort is fail-stop.

## Validation scope

The equivalent native suites cover 35 fail-stop misuse scenarios on POSIX,
8,192 slot reuse cycles, 8,192 exclusive-loan reuse cycles, 64 simultaneous
readers, disjoint-slot borrowing, carrier migration, isolated storage even with
equal numeric owner/incarnation, and retirement with outstanding loans.

On Linux/GNU-compatible linkers the test wraps malloc/calloc/realloc/free and
forbids those calls during guarded arena operations. This says nothing about
allocations in the rest of the host, the scheduler, guest programs, or threads
outside the guard. Positive tests still run on non-POSIX hosts; fork-based
negative tests require POSIX. Checks use explicit failure paths under NDEBUG.

Local GCC 13 optimized builds passed for both implementations. The shared
arena also passed AddressSanitizer/UndefinedBehaviorSanitizer with leak scanning
disabled because the execution environment prevents LeakSanitizer's /proc
thread inspection. Hosted exact-head CI and CMake integration remain separate
checks; a local pass is not their result.

## Deeper compiler and runtime integration

LLVM source ownership previously skipped ActorDecl.on_message entirely. The
checker now analyzes handler argument order, explicit moves and call loans,
rejects reuse of moved message values, and requires actor fields to be restored
before returning if explicitly taken. Immutable state cannot be taken. Actor
field mutable borrows still fail until handle-aware writeback lowering exists;
shared call-scoped reads and mutable borrows of the message parameter are
checked using the ordinary function rules.

A second compiler fix preserves all emitted actor callbacks as implicit AOT
reachability roots. Previously a helper referenced only by on_message could be
removed by --tree-shake. Its direct/transitive name+arity dependencies now stay
in the closure. Unused ordinary functions are still removable. Actor declaration
pruning is not implemented, so all emitted actor callbacks must be retained.

The real compiler scalar field ABI, ActorOwnedI64Heap, now checks physical
carrier ownership, supports shared/exclusive host loans and requires first-turn
fixed-field initialization. A preallocated free list makes dynamic slot
allocation/drop O(1); fixed-field reservation can scan during construction.
This is the heap used by SourceActorBootstrap, independently of the generic
byte-slab arena introduced in the first slice.

Forge ScalarActor binds that same scalar heap ABI to a preallocated int64 FIFO.
try_send returns Accepted/Full/Closed/Foreign without allocating. run_one admits
one callback at a time and returns Busy to concurrent carriers. Callbacks run
without holding the mailbox/lifecycle mutex, so send and stop stay responsive.
stop closes ingress and drains accepted messages; callback failure seals the
actor and wipes queued values. References carry per-instance identity, including
when a numeric actor id is reused. Construction reserves one heap without executing actor code. The first admitted
worker turn runs initialization before its first message; an external pool
schedules all turns. Stop before startup can cancel without running initialization.
The runtime creates no threads and performs no interpretation/JIT compilation.
Destruction requires external callers to be fenced and callbacks to release all
loans; it rejects unresolved heap holders rather than freeing live storage.
This is trusted-host callback integration, not Forge guest actor compilation,
actor suspension, Erlang-style supervision, or untrusted execution isolation.

Executed optimized checks include 15 admitted + 23 rejected source programs,
11 actor AOT reachability checks, 29 scalar-heap misuse rejections per backend,
full 4096-slot occupancy with reserved fields, and 16384 scalar slot/loan reuse
cycles. The actual OresVM/source bootstrap runs 1600 host-callback messages
across 16 distinct heaps and reclaims each heap. Forge tests exercise 512
messages contended across four producers/four workers, stop during an active
callback, FIFO/carrier migration, failure cleanup, stale-reference rejection,
and 16384 turns guarded against C allocator and ordinary C++ new/delete calls.
No-LLVM scripts execute the actual frontend/ownership sources and native runtime
in optimized and ASan/UBSan modes. They do not build LlvmCodegen or execute
new .ores-derived machine code; that still needs the full LLVM toolchain.

## Next implementation priorities

1. Extend the existing LLVM scalar actor-owned state path to aggregates, and
   connect Forge source actor syntax to its new host runtime without weakening
   ownership, lifecycle or actor-incarnation checks.
2. Emit the same immutable/exclusive borrow policy from typed IR on each
   backend. Check branch joins, projected fields, use after move and call
   arguments; reject borrowed actor messages and unsupported loan escapes.
3. Lower await to owned continuation frames. Persistent owners/leases may pin
   storage, but lexical mutable state loans must not escape suspension without
   a proven safe lowering and actor-state exclusion.
4. Define a strict guest-AOT + zero-GC admission profile that rejects unsupported
   reachable allocation, reflection, runtime code loading, guest evaluation and
   tracing dependencies before publishing an artifact.
5. Generate and execute real guest actor programs with heap isolation,
   ownership transfer, cancellation and exact-artifact allocator evidence.
   Preserve optional alloc/actor/stdlib layering; no mandatory heap in core.

Immutable code can be shared per code version. Mutable heaps remain actor-owned.
Cross-actor immutable data requires an explicit authorized sharing domain.
