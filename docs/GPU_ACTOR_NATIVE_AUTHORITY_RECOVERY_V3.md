# GPU actor native completion authority and supervised recovery V3

**Draft, host-only security milestone.** This proposal stacks on LLVM draft #1672. It does not enable guest `define gpu actor` execution or prove a physical GPU event, and its worker design is not crash durable.

## P0: no legacy V0 Boolean completion seam

The conditional `ORESLANG_GPU_ACTOR_V0_HOST_TEST_SEAM` is removed from the production V0 header. `HostTurnBridgeV0` retains private methods for prepare/enqueue/abort, completion, cancellation acknowledgment and buffer retirement. Only the friend V1 supervisor-owned registry can call them. Ordinary native callers remain compile-negative through `tests/gpu_actor_v0_completion_authority_negative.cpp` and the persistent CMake access gate.

The former direct V0 regression executable no longer calls the legacy unsafe functions. Its source file, `tests/gpu_actor_host_turn_bridge_v0_test.cpp`, has been **replaced** by registry-driven coverage for 64 actors, 4 turns/actor, state-visibility ordering, cancellation, ownership generations, checked overflow and correct supervisor CONTROL retirement. **The earlier 6,257 V0-direct assertions are replaced, not retained.** The independent 6,329-check V1 actor/quarantine suite continues to exercise the shared underlying V0 host code.

This is source-level C++ API restriction, not cryptographic authority or a guarantee against malicious linked native code. Provider implementations remain trusted and require hardware validation.

## P1: worker isolation from actor scheduler mutex

`GpuActorOpenClRecoveryWorkerV1.hpp` provides a bounded, in-process, supervisor-owned, single-threaded native work queue. Each request is tied to a specific provider pointer, actor generation, turn sequence and recovery kind. It rejects stale/foreign turns, coalesces duplicate requests and refuses over-capacity work. Queue capacity is explicit (1–4,096). Jobs hold the provider and supervisor registry through the native call.

Four privileged recovery methods are supported. These V2 entrypoints, plus recovery-turn identity validation, are now **private** and callable only by the friend `OpenClActorRecoveryWorkerV1`. A compile-negative source and an additional CMake/CTest gate require GCC and Clang to reject any ordinary translation unit that attempts to invoke them:

- ambiguous upload/begin after-accept failures
- ambiguous kernel/download enqueue
- device-loss queue quiescence
- release-failed retry

The V2 adapter reserves `recovery_in_progress_` under a short mutex acquisition and then calls the native provider **without holding the adapter mutex**. This matters because an OpenCL `clFinish` may block indefinitely. Actor `observe()`/reconciliation returns Unknown while recovery runs instead of waiting for the worker's lock. Once the provider reports actual quiescence and cleanup, the worker marks a **failed** native completion; the actor scheduler later polls the V1 registry and publishes the appropriate supervisor CONTROL notification. The worker itself never publishes actor state, releases loans based on a timer, or manufactures `CompleteOk`.

Tests deliberately block a fake native queue-finish call on the worker thread while an actor registry poll executes concurrently. A one-second testing deadline checks scheduler responsiveness only; **it is not a proof of device quiescence**. The test also checks duplicate-key coalescing, bounded queue rejection, stale-turn rejection, no staged-state commit, failed-event notice, and eventual cleanup after a trusted fake completion.

**Open blocker:** This worker is an in-process C++ thread. Its destructor joins the thread; if a GPU driver never returns from `clFinish`, the supervisor's shutdown can hang. Resource quarantine is process-local, not crash durable. Production requires a separately supervised helper process or equivalent bounded execution domain with per-device quarantine lifetime and restart/rebind protocol; no forced GPU memory reclamation is safe without a device-reset or quiescence certificate. Do not claim liveness or hard-real-time guarantees from this implementation.

## Verification

Run the exact source through token-free private GitHub connector mirroring to `scintilla-run/scintilla-run-e2e`. Verify all mirrored Git blobs before compilation. Execute:

- GCC C++17 `-Werror` plus ASan/UBSan and independent Clang host tests
- original V1 1,024-actor/4,096-turn lifecycle suite
- replaced V0 registry migration tests
- V2 OpenCL provider mock and stalled worker test
- preexisting OpenCL G3 provider state-machine suite
- compiler-negative V0 Boolean access tests and reusable CMake negative gate
- opt-in `ORESLANG_GPU_ACTOR_HOST_ONLY=ON` actual repository **six** CMake/CTest host targets, including the new worker-authority denial gate
- native physical-GPU prerequisite probe, with exit 77 counted as NOT_EXECUTED

The host-only CMake mode builds real repo files but deliberately returns before full LLVM/compiler configuration. It must never be reported as full source-repository compiler/guest CTest.

## Remaining release gates

1. Independently authenticated driver/device/queue event identity and physical GPU execution on an authorized GPU-equipped runner (no CPU fallback).
2. Separate supervised process for potentially hanging native GPU recovery and crash-durable or safe-reset quarantine.
3. Close remaining provider and FFI trust bypasses, including proof obligations for any dynamic C++ plugin or external library.
4. Compiled GPU actor body HIR/MIR, ownership and device-effects validation, runtime kernel dispatch and full-AOT guest execution.
5. Full compiler/build CTest and five-backend conformance, with unsupported platforms explicitly failing closed.

**Draft only; do not merge or declare production-ready without independent review.**
