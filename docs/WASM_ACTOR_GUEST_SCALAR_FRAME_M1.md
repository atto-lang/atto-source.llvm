# LLVM-to-Wasm actor guest scalar frame M1 — checked per-instance storage

**Status:** compiler/codegen implementation and authored acceptance tests. Exact-head compiler-backed test execution NOT_VERIFIED until admitted CI runs.

## Backend problem and narrow repaired slice

The existing wasm32 actor handler in `LlvmCodegen.cpp` materialized the incoming i64 message argument using an LLVM `alloca` (`message.slot`) in the Wasm engine function call stack. A host-side 64KiB actor-managed frame arena does not put the **guest argument** into an actor-owned linear-memory frame.

This increment creates two **internal, per-WebAssembly.Instance Wasm globals** for the restricted synchronous scalar actor module:

- `oreslang.wasm.actor.frame.depth`: private 64-bit frame count, initially zero.
- `oreslang.wasm.actor.frame.slots`: private `[64 x i64]` slots (512 bytes, within the already admitted fixed two-page guest memory).

The handler checks `depth < 64` before creating an inbounds slot address, writes the source message parameter into the current slot, **volatily reads it back**, and copies that read value into the existing restricted scalar evaluator. The handler must zero the guest slot, restore the prior depth, and return only after completing all field writes. The compiler emits explicit volatile stores/loads so Clang `-Oz` cannot erase the guest-memory accesses. On overflow the compiler emits `llvm.trap` and `unreachable`, not an unbounded memory access.

The guest lifecycle initializer sets depth to zero, and teardown refuses a live guest frame. Internal globals are not exported; the existing three actor exports and fixed two-page memory ABI do not grow. Each actor receives its own WebAssembly.Instance and so owns independent backing memory. No host pointer, import or source-derived authority can alias another actor's frame slots.

## What this does NOT do

This is **one actual source-compiled scalar argument placed in guest-owned managed frame memory**. The scalar evaluator still carries one transient parameter copy in its Wasm engine activation. It is NOT full compiler-lowered return PCs, roots, nested calls, recursion, suspension/await, ownership, native call-stack isolation, or host-independent actor STOP/CONTROL. The current compiler fails closed for unverified actor nested calls. The 64-frame limit is a future ABI ceiling and **cannot be exercised as real recursion** through the current one-call synchronous actor subset.

The WebAssembly runtime's own native call stack is still bounded only by its host engine. Do not mark actor A stack overflow / co-located actor B survival as proven. A genuine compiler-managed iterative/trampoline actor call path must be added separately, with supervisor failure outcomes and cross-backend fixtures.

## Required exact-head native guest tests

The new CTest `wasm32_compiler_owned_actor_guest_frame_scalar` invokes `tests/wasm32_actor_owned_guest_frames.py` and:

1. Pins source blob `adfc0b43463d7ce92d32a4b1b63602f91e527329` shared exactly by the legacy `.ores` file and new canonical `.atto` source.
2. Runs the actual newly built `oreslang-llvmc --target=wasm32` on `.atto`; checks generated target IR for the [64 x i64] guest frame global, bounded depth guard, real GEP, volatile write/read/scrub and teardown guard; rejects an old `message.slot` alloca.
3. Links that exact IR using Clang wasm32 **O0 and Oz**, fixed two-page max, no imports, and strict three-lifecycle-function plus memory export ABI.
4. Uses Node's actual WebAssembly.Module/Instance engine to execute 100 increment messages on actor A, independent actor B, STOP A with terminal access denial, B's continued progress, and fresh actor C with no inherited state. Emits actual fixture/compiler/Wasm SHA-256 witnesses per optimization variant.
5. Does not count synthetic IR, host-only arena tests or missing compiler as a pass.

CI pass requires a real runner ID, nonempty executed job steps and exact source HEAD/tree evidence. `steps=[]` or `steps=null` indicates admission failure. The source organization's GitHub Actions billing/payment-or-spend-limit condition must be fixed by an authorized account owner, without exporting private source or weakening security review.

## Next milestone — real recursive actor containment

Lower source actor function calls and returns to an **iterative** actor-owned frame program-counter and operand layout. Enforce an actor-local quota before every push and generate an uncatchable typed supervisor-facing `STACK_BUDGET_EXHAUSTED` status, not merely a generic Wasm `unreachable` trap. Run a canonical source fixture with actor A reaching the 65th frame, actor B continuing on the same carrier, and supervisor restart fencing all stale capabilities; compare real Java/LLVM/Wasm guest traces at independently pinned heads.
