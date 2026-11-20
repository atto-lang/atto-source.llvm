# GPU actor OpenCL uncertain-enqueue security audit — 2026-11-10

**Status:** draft security hardening, exact-source native host tests passed. No physical GPU event, compiled AttoLang guest, or full AOT guest dispatch was executed.

## High-severity defects found

In the existing `OpenClProvider::begin()`, `durable_enqueued` was set only after `Api::enqueue_upload` returned a non-null event. An exception or null event after the native driver accepted a write could fall through `abort_gpu_before_enqueue` and `finish_cleanup`, releasing buffers and pinned host staging before device quiescence. A similar hazard occurred when `poll()` attempted kernel or download enqueue after the previous event completed: a driver could accept the next command but throw before returning its `cl_event`, while the host previously marked the pair failed and released memory.

These are unsafe because failure of a *host API call* does not establish failure to *physically enqueue* a device operation.

## Hardened ownership transitions

The runtime now records the boundary **before calling** each native upload/kernel/download enqueue function. Any error or missing event after that boundary is treated as `Unknown`, never a definitive no-enqueue receipt. The exact `OpenClProvider::Operation` remains in the bounded native quarantine with its physical allocations, quota, typed GPU buffer fences, and host staging bytes still retained.

For uncertain kernel/download launches, a dedicated `UnknownEnqueue` state prevents a follow-up poll from dereferencing a missing event and prevents cancellation from falsely completing an unknown command. The V2 actor bridge maps this state to a supervisor HostFailure/retained-loan path, not to a successful completion. Earlier completed upload/kernel stages are not replayed.

Two privileged, synchronous **recovery-worker-only** entrypoints now exist:
- `recover_ambiguous_begin_after_quiescence()`: after a trusted provider proves the OpenCL queue is quiescent, aborts the upload-only host tickets and retires physical buffers/quota. An upload might have happened; the actor kernel did not execute. This is not a claim that no physical upload was enqueued.
- `recover_ambiguous_kernel_after_quiescence(operation)`: after queue quiescence proof, acknowledges canceled device tickets, frees the exact buffers and releases quota; the actor never publishes staged state.

A native recovery worker may call the blocking provider queue-finish operation (`clFinish` on the system adapter). **Never call these methods from an actor scheduler polling turn**: the driver may hang or time out, and timeout alone is not proof of quiescence. Scheduler `reconcile_unknown` remains nonblocking and returns Unknown until the worker has produced the required receipt.

The V2 port issues an actor-bound, locally authenticated **failed terminal receipt** only after the worker proves quiescence and physical cleanup. It does not forge `DefinitelyNotSubmitted`, nor issue a successful device event. Event observation validates provider-instance provenance and actor/turn identity; the local token is invalidated when the physical provider retires. Earlier post-recovery event handling could strand a genuinely quiescent operation because it required a live operation pointer; this was fixed and exercised by the tests.

## Executed exact-source tests

Private token-free Git-blob carrier: https://github.com/scintilla-run/scintilla-run-e2e/actions/runs/42952096758

All source blobs mirrored through the connected GitHub API into a temporary private carrier branch. No cross-org `GITHUB_TOKEN`, personal token, private checkout secret, or source-org Actions minutes were required. The workflow checks `git hash-object` against the original source blobs before compiling.

- GCC C++17 strict `-Werror` with ASan/UBSan: **128 V2 checks passed**
- Clang C++17 strict: **128 V2 checks passed**
- Inherited V1 actor/provider lifecycle tests: **6,329 checks passed on each compiler**, including 1,024 logical actors and 4,096 mock-provider turns
- Preexisting OpenCL G3 native provider state-machine test suite: **passed with GCC/ASan/UBSan**
- Negative tests cover exception **before and after simulated GPU API acceptance** for upload, kernel and download; staged actor state never commits; no physical-buffer release occurs until a trusted queue-quiescence receipt; retired provider events cannot replay.
- Native actor→physical OpenCL integration probe compiled and was attempted: **exit 77**, `GPU_ACTOR_V2_NO_GPU_RUNTIME no-compatible-GPU; no CPU fallback`. This is **GPU_NOT_EXECUTED**, not a hardware pass.

Mocks deliberately simulate an API returning after work was *accepted*, but **they do not authenticate or execute actual hardware commands**. They exercise production native provider code using a controlled `opencl::Api` seam.

## Outstanding release blockers

1. **Legacy V0 native completion Boolean methods remain callable by trusted host code.** No production guarantee that all paths must go through V2 physical proofs until this privileged seam is removed or access-controlled. This audit intentionally does not break unrelated V0 test consumers by rewriting that API without a migration.
2. Native `OpenClSystemApi` still relies on its driver to provide correct `clFinish` quiescence; there is no independent driver-event attestation or process-reset proof.
3. Quarantine is bounded but **not crash-durable**, and a stalled GPU can retain device memory indefinitely. The blocking recovery method needs a dedicated supervised worker with process-root lifecycle management and cancellation policy; scheduler calls are forbidden.
4. No GPU-equipped hardware runner was available. Production cannot be enabled until real GPU-only execution, event/queue identity, numerical oracle and cancellation/device-loss tests pass with recorded driver/device information.
5. No compiled `define gpu actor` body MIR, guest AOT kernel dispatch, full integration CTest, five-backend parity or supervisor-tree crash recovery was executed.

**Keep draft; do not merge or claim GPU actor production readiness.**
