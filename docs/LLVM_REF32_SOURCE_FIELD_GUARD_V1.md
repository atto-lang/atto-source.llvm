# LLVM ref32 source-field validation before memory loads

**Status: LLVM codegen helper + generated LLVM-IR regression.** Not yet
source-compiler general actor lowering or a native adversarial sandbox.

## Why this matters

`ManagedRefLowering.hpp` already validated an encoded **target** reference
against an original-generation proof and actor arena bounds before dereferencing
the target. But the emitted code read the **source field** (`i32 ref32`)
first. A stale, foreign or malformed source-object pointer could therefore
read an unrelated address before the target's proof callback was consulted.

## Incremental change

The helper now inserts a new `ref32.source.bounds` basic block before
`ref32.source.read` for **both loads and stores**. Generated IR enforces:

- source field address is not below the current actor arena base;
- at least four bytes of source field fit within the declared arena span;
- source field pointer is four-byte aligned, base is non-null/eight-byte aligned;
- declared arena span fits the target's flat 32/64-bit addressing model
  without crossing the native address ceiling;
- source-pointer comparison, subtraction, and range checks occur in widened
  i64 arithmetic before the first `load i32`.

Only then may the generated code read `ref32.encoded` and enter the
existing original-generation proof, target-span validation and native
load/store blocks. Invalid pointers cause the existing `llvm.trap` branch.

This does **not** prove the source field is a live allocated object. It
ensures architectural source-field range/alignment checks, not complete
pointer provenance or speculative-execution secrecy. The source field still
requires an allocation-lifetime proof linked to actor turn ownership; the
original target proof alone cannot supply that provenance. Real `.atto`
compiler lowering and native/FFI restrictions remain separate pending gates.

## Evidence classification

- `tests/managed_ref_llvm_lowering_test.cpp` checks generated IR ordering,
  target native pointer widths and source guard presence on four target
  DataLayouts (native64, i686, armv7, wasm32).
- `tests/managed_ref_llvm_e2e_harness.cpp` passes a real valid-but-foreign
  source object containing a plausible compressed reference to generated
  native code, and expects an explicit trap **before** target load/store.
  Misaligned sources and existing stale/null/overflow target probes remain.
- `.github/workflows/llvm-ref32-source-guard.yml` is an exact-head
  LLVM/Clang/Rust native-generated-IR O0/O2 gate. It does not build the
  production `oreslang-llvmc` `.atto` frontend or establish a complete
  untrusted TenantVM.

Pending actual executed test steps, status is **NOT_EXECUTED**. Zero-step
GitHub Actions failures are runner admission failures, not a compiler or
sanitizer verdict.

## Remaining priorities

1. Require compiler-emitted source-object liveness, generation and owner
   proof before reading even an in-arena object field. A pointer into freed
   or never-allocated arena space must not pass.
2. Bind the proof ledger to the TenantVM+actor+physical-turn context and
   validate across suspend/resume, retirement, arena migration and restart.
3. Integrate this helper into the actual native source compiler; add
   byte-identical `.atto` guest positives/negatives and verified exact-IR
   path coverage that rejects an unchecked `GEP` or FFI escape.
4. Preserve native zero-tracing-GC and forbid guest raw pointer exports.
   A separate process/Wasm isolation worker remains required for genuinely
   hostile native workloads; MPK is an additional defense, not substitute.
