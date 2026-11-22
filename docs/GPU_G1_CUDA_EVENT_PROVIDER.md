# AttoLang GPU G1 — CUDA Driver API event provider (draft)

**Evidence status: HOST_ONLY + CUDA_API_SOURCE_ADAPTER.** This change has no authenticated physical GPU execution and does not enable general `.atto` GPU launch semantics. It is stacked on the provider-owned host ledger PR #1508, itself stacked on #1506 and the deliberately restricted `.atto` SAXPY compiler #1473. No CPU fallback is allowed or claimed.

## Trust boundary

- `GpuG1EventProvider<Driver>` (header `GpuG1EventProvider.hpp`) is a **trusted host embedding interface**, not guest-accessible `.atto` surface. `GpuG1CudaDriver` (`GpuG1CudaDriver.hpp`) binds this interface to real CUDA Driver API `CUcontext`, `CUstream`, `CUdeviceptr`, `CUfunction`, and `CUevent`.
- Only a trusted host may supply a context, device and **vetted compiler-produced SAXPY `CUfunction`**. The adapter only checks equality to that function pointer. It does **not** authenticate the PTX/module SHA-256 or prove compilation from a pinned exact-head `.atto` source. The module and CUDA context must outlive all streams, allocations and in-flight events.
- The shared `DeviceLeaseRegistry` must be controlled by the same trusted provider and its event-ack methods must not be exposed to an untrusted guest, a callback controlled by a guest or a separate untrusted native plugin. C++ types alone are not an OS security boundary. Every tenant sharing one physical device must use a coordinated device-wide quarantine domain.

## Lifecycle and invariants

1. **Allocation and queue identities** derive from successful driver calls (`cuMemAlloc`, `cuStreamCreate`) followed by registry admission. The registry mints immutable handle generations; the provider checks exact registry incarnation, tenant/device identity, queue/allocation ID and generation. Host tracking failures roll back admitted handles and free newly obtained driver resources where safe.
2. **Device launch:** validate the trusted kernel identity, finite f32 scalar, f32 byte alignment, checked `n * 4` range, 32-bit CUDA grid/block ABI and device-published bounds. An empty logical extent performs zero dispatch and returns ticket 0. For a nonempty launch, create a CUDA event, atomically acquire read/write loans in the registry, enqueue the restricted SAXPY `cuLaunchKernel`, then `cuEventRecord` in the same stream. Memory/queue loans stay held while the event is pending.
3. **Async completion:** `poll(queue,ticket)` calls the driver `cuEventQuery` only; it does not call `cuCtxSynchronize` or `cuStreamSynchronize`. `CUDA_ERROR_NOT_READY` leaves the loan held. Only a CUDA-reported completed event permits `provider_ack_event`, driver event destruction and loan release. `request_cancel` records intent; a cancelled task still holds its loan until the event reports genuine completion/quiescence.
4. **Failure/teardown:** a driver launch/record/query failure or unknown event state quarantines the device and retains loans. There is no synthetic completion, guessed cancellation, forced release, implicit CPU execution or automatic unquarantine. `release_allocation` and `close_queue` must prove zero in-flight loans; `require_quiescent_shutdown` refuses outstanding resources. A quarantined context requires a separately governed cleanup/recovery path; leaked/held GPU resources must not be returned to another tenant until quiescence is independently established.
5. **Scheduling:** the provider mutex and driver allocation/free APIs belong on a control/device-service lane. Do not call these potentially blocking operations from the actor scheduler carrier or inside `critical`. An AttoLang `Future<void>` wakeup adapter and a bounded/fair runtime event pump remain **not implemented**; the current `poll` is a nonblocking building block only.

## Tests and evidence classification

- `tests/gpu_g1_event_provider_test.cpp`: **3,292 local host-only assertions**, including 512 deterministic event/cancel/lifecycle rounds, 16-thread exclusive-writer race, invalid aliases/bounds, queue closure, duplicate/stale completion, cross-registry and cross-tenant rejection, device-wide quarantine and invalid launch/event failures. The fake driver is deliberately **TEST_ONLY**, not a hardware device.
- Local GCC and Clang C++17 strict `-Wall -Wextra -Wpedantic -Werror -UNDEBUG -pthread` and Clang ASan/UBSan suites passed against the exact provider/test bytes. A separate test-only stub `cuda.h` (NOT checked in) allowed C++ API-surface syntax checks with both compilers; this is **not a real CUDA SDK build**.
- `ATTO_GPU_G1_CHECK_CUDA_SDK=ON` is an explicit CMake opt-in to compile/link `tests/gpu_g1_cuda_adapter_compile_test.cpp` with a real installed CUDA SDK (`CUDA::cuda_driver`). The default OFF cannot accidentally report physical execution. Hardware execution still requires a separately authorized CUDA runner.
- The G1 exact-head workflow hashes and runs the fake-event suite, classified `HOST_ONLY`. A job with `steps=null` or `[]` means runner admission failure, not a test pass or test failure.
- Test and implementation events are **not physical GPU evidence** until independently authenticated `.atto` source SHA + compiler artifact SHA + CUDA device identifier and event receipts + actual numeric/redzone oracle results all bind to an exact PR head and executed job steps.

## Remaining milestones / merge hold

- Validate `GpuG1CudaDriver` against genuine CUDA SDK and a real NVIDIA context/module on an independently authorized hardware host; no such execution has been completed by this change.
- Connect restricted G1 source compiler output from #1473 to verified CUDA module/function loading. Do not accept arbitrary guest-provided PTX or module pointers.
- Implement scheduler event pump and structured `Future<void>` cancellation/ownership handoff, plus trusted provider shutdown/driver-loss recovery and device-scoped tenant registry lifetime.
- Execute real pinned-head SAXPY with independent elementwise numerical oracle and redzones; bind receipts to conformance #549 without promoting a self-reported JSON schema.
- Require executed exact-head hosted CI, security review, ownership audit, branch protection and explicit merge authorization. Do not merge this stack based on host-only evidence.
