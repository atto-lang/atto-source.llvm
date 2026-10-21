# Native pointer substrate and pointerless source parity

Status: host/runtime lifetime hardening + permanent fail-closed source gate,
2026-10-21.

Oreslang source does **not** expose `Pointer<T>`, `RawPointer<T>`,
`rt ref`, `rt deref`, `rt unref`, `rt raw`, address-of, or explicit
dereference syntax. The native frontend rejects those forms before LLVM IR.
Native pointer/handle representations are trusted backend/runtime details only.

## Why this is fail-closed

A raw native payload pointer is not sufficient to prove that a handle is
live or belongs to the requesting actor. The existing native
`ActorOwnedGraphHeap` prototype already distinguishes
`ActorHeapIdentity {runtime,actor,epoch}` and generation-fenced slot handles.
It is **not** yet a source-language pointer implementation: object lowering,
drop glue, lifetime/CFG analysis, FFI pinning and actor/async control-plane
integration are separate acceptance gates.

Historical Java public-pointer drafts have been superseded by the pointerless
source contract. Do not use them as a language/API target. Preserve only
backend/runtime lifetime, provenance and retirement lessons that remain useful
for internal native handles.

## Audited native heap substrate: borrow versus mutation

This draft also hardens the **host-only** `ActorOwnedGraphHeap` substrate:

- `ActorHeapBorrowGate` now reserves a writer bit atomically against every
  shared FFI/continuation lease. New leases are rejected while an exclusive
  mutation is active, and mutations are denied while any lease exists.
- Allocation, construction, byte writes, graph-link changes and deletion
  hold an exclusive stack-scoped admission ticket. Metadata/payload reads
  hold shared tickets. Underlying storage cannot be retired during either.
- Destruction **seals admission first**, then fails-stop if loans remain.
  Retirement is single-start even under concurrent calls. This is not
  a substitute for external quiescence of owners of the heap object itself.
- Heap identity reservation is monotonic and nonwrapping **across all
  template arena layouts**, not per-template; generation-checked slot
  recycling remains separately required.
- The gate is conservative **heap-wide**, not per-slot. It may reject a
  logically independent writer while any exported read exists. This is the
  correct fail-closed behavior until path-aware loans and object-local pins
  can be proven end to end.

### Verification provenance

The exact Git-blobs of `ActorHeapIdentity.hpp`,
`ActorHeapRetirementProof.hpp` and
`actor_heap_borrow_mutation_gate_test.cpp` were independently compared
with their Git SHA-1 blob hashes before compiling the **gate-only**
test in a local sandbox. It passed Clang ASan+UBSan, GCC Release (30 runs),
and GCC ThreadSanitizer. This validates the loan gate only. The
`ActorOwnedGraphHeap.hpp` integration changes, Linux carrier test, full
LLVM compiler and source guest execution have **not** yet had an executed
CI/local full build on this commit; GitHub Actions on the source org ended
before reporting any executed steps.

## Internal native-handle requirements

1. Backend/runtime handles must retain unique provenance, generation/epoch,
   bounds, alignment and actor/allocation authority.
2. Native read/write access must validate the live internal handle before
   forming an ephemeral host pointer; no naked pointer is exposed to guest
   source.
3. Internal ownership moves and retirement/drop obligations must be
   single-consumer and fenced against outstanding loans/pins.
4. Actor message/channel/stream handoff must transport only source-level owned
   values/capabilities proven by the ordinary ownership system; it must not
   expose native handles or pointers.
5. Async/FFI/device leases inside core/runtime must include cancellation ACK,
   producer quiescence and generation fencing before storage reuse.
6. Exceptional cleanup must cover throw/raise/panic, defer/finally,
   cancellation, retirement and hot reload without creating a guest pointer
   escape hatch.

## Measurements and acceptance

- The phrase *zero copy* describes absence of copying `T`'s backing bytes.
  The phrase *zero allocation* additionally requires no heap allocation of
  handle wrappers, refcount cells or queue nodes. These require **separate**
  instrumentation and benchmarks.
- Keep negative source fixtures for pointer-style spellings in Java and LLVM.
  They must continue to reject before LLVM IR/artifact publication; no positive
  guest pointer fixtures are expected.
- Run exact-head LLVM build and CTest, ASan/UBSan and concurrency stress;
  exercise ABA, forged/foreign handles, cross-actor access, loan-vs-release,
  release-vs-cancel, double drop and resource exhaustion.
- Never infer zero-GC from this native prototype; only a verified execution
  profile with ownership-lowered guest allocations qualifies.

Related: `docs/HYBRID_MEMORY_MODEL.md`,
`docs/JVM_LLVM_OWNERSHIP_PARITY_GATES.md`,
`src/main/cpp/oreslang/runtime/ActorOwnedGraphHeap.hpp`, and Java
`docs/HYBRID_MEMORY_MANAGEMENT.md`.
