# Native Pointer<T> admission and ownership parity

Status: integration gate and regression design, 2026-10-15. **No source-level
Pointer<T> native lowering is implemented on main.** The parser now explicitly
reserves `rt ref`, `rt unref`, and `rt deref` and rejects them before LLVM
IR emission. See the corresponding Java current-main draft
`docs/POINTER_BORROW_AND_ZERO_COPY.md` for the proposed language contract.

## Why this is fail-closed

A raw native payload pointer is not sufficient to prove that a handle is
live or belongs to the requesting actor. The existing native
`ActorOwnedGraphHeap` prototype already distinguishes
`ActorHeapIdentity {runtime,actor,epoch}` and generation-fenced slot handles.
It is **not** yet a source-language pointer implementation: object lowering,
drop glue, lifetime/CFG analysis, FFI pinning and actor/async control-plane
integration are separate acceptance gates.

Historical Java draft
https://github.com/ores-truffle-oreslang/oreslang-source.java/pull/443
implements managed pointers, but has materially diverged from `main`.
It also defines `rt unref` as borrowed read access, identical to
`rt deref`. The integration contract proposes separating explicit release
from read-only dereference; decide semantics before enabling *either* backend.

## Native lowering requirements

1. `rt ref` moves unique ownership of an existing T into a typed affine
   owner handle. It must not duplicate owned storage or manufacture a
   foreign-region token. Handle construction alone must not imply a new
   allocation.
2. `rt deref` checks live slot, generation, actor/arena epoch, bounds and
   alignment; produces a nonescaping shared borrow tied to that exact root.
   No naked `T*` may be returned to guest source across a suspension.
3. `rt unref` under the proposed release semantics consumes the unique
   handle and runs the correct drop/retirement path **once**, only when no
   derived loan/pin is outstanding. The exact Java/native semantic choice is
   a blocker, not yet an implemented contract.
4. `rt take` moves the handle and its drop obligation. `rt copy` and
   `rt share` of a unique pointer fail closed until separately proven
   copy/shared ownership protocols exist.
5. Actor message/channel/stream/observable handoff transports **owned**
   buffers with atomic admission; it must reject borrowed handles and
   pointers. A future pinned async buffer lease requires cancellation ACK,
   producer quiescence and a generation fence.
6. Exceptional cleanup must cover throw/raise/panic, finally/defer, failed
   channel admission, cancellation, retirement and source generation changes.
   The native runtime must not reuse a slot while any lease is alive.

## Measurements and acceptance

- The phrase *zero copy* describes absence of copying `T`'s backing bytes.
  The phrase *zero allocation* additionally requires no heap allocation of
  handle wrappers, refcount cells or queue nodes. These require **separate**
  instrumentation and benchmarks.
- Add identical positive and negative source fixtures in Java and LLVM once
  checked lifetime lowering exists. The current negative matrix verifies
  explicitly gated syntax emits **zero LLVM IR**.
- Run exact-head LLVM build and CTest, ASan/UBSan and concurrency stress;
  exercise ABA, forged/foreign handles, cross-actor access, loan-vs-release,
  release-vs-cancel, double drop and resource exhaustion.
- Never infer zero-GC from this native prototype; only a verified execution
  profile with ownership-lowered guest allocations qualifies.

Related: `docs/HYBRID_MEMORY_MODEL.md`,
`docs/JVM_LLVM_OWNERSHIP_PARITY_GATES.md`,
`src/main/cpp/oreslang/runtime/ActorOwnedGraphHeap.hpp`, and Java
`docs/HYBRID_MEMORY_MANAGEMENT.md`.
