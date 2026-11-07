# GPU-G2: CUDA Driver API provider and physical SAXPY gate

**Status: unmerged stacked draft, physical CUDA NOT_EXECUTED.** This extends the G1 source-to-PTX compiler draft #1469 on top of the GPU source-admission draft #1457. The ordinary `.atto` compiler and generalized GPU effect/borrow lowering are not enabled here.

## Actual implementation

- `src/main/cpp/oreslang/gpu/G1CudaProvider.hpp`: single-flight host driver contract and state machine. One stream, one `saxpy` entry, three distinct allocation-generation identities, bounded vectors, exact provider-reported geometry, three `GpuBufferFence` owners and independent sealed tickets.
- `src/main/cpp/oreslang/gpu/G1CudaDriver.hpp`: optional CUDA Driver API adapter using `cuda.h` and `CUDA::cuda_driver`, `cuModuleLoadData`, `cuModuleGetFunction`, `cuMemAlloc`, `cuMemcpyHtoDAsync`, `cuLaunchKernel`, `cuMemcpyDtoHAsync`, `cuEventRecord/cuEventQuery`, `cuStreamQuery`, and explicit resource retirement.
- `tests/gpu_g2_cuda_provider_host_test.cpp`: fake driver tests for failed admission, scalar output oracle, launch order, empty launch, event pending, cancellation, no premature free, driver enqueue failure, event creation failure, and recoverable release failure.
- `tests/gpu_g2_cuda_hardware_test.cpp`: **opt-in physical CUDA** integration test for `n=0,1,255,256,257,1024,4097,16385`, numerical oracle and cancellation after submission.

### Exact access boundaries

**The provider is a restricted hardware-test adapter, not production actor scheduling.** Its fake driver does not authenticate a real device; only the real CUDA driver resolves device limits and actual PCI location. The current PTX admission uses a structural kernel-entry check, **not a cryptographic source-to-module attestation**; therefore it must never accept guest-controlled PTX or become a general multi-tenant launch endpoint. It expects locally generated trusted PTX at the hardware test boundary. The host staging uses ordinary pageable vectors with CUDA async copy calls, which can block the host during submission; it is not an async AttoLang carrier API. Fix these limits before exposing to guest programs.

The operation destructor fails closed (terminates the host process) if live device resources remain. This avoids freeing host staging/device handles while a DMA or kernel is still pending, but is **not production-grade recovery**. A real runtime must retain orphaned operations in a supervisor-owned device service and implement bounded quarantine/recovery, not rely on termination.

**State transitions:** `Ready -> Submitted -> Completed`; `Submitted -> CancelRequested -> Cancelled` only after stream quiescence; failed enqueue or unknown event becomes `Quarantined`; failed cleanup becomes `ReleaseFailed` and is retryable. All device buffers are retired only after authenticated event success or successful stream-quiescence proof. A device error that never proves quiescence does not unlock device memory. Exactly one operation is permitted per object.

## Execution commands and evidence

Host-only fake-driver test (no CUDA headers or GPU required):

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --target oreslang-gpu-g2-provider-host-test
ctest --test-dir build -R '^native_atto_gpu_g2_cuda_provider_HOST_ONLY$' --output-on-failure
```

Physical CUDA (requires an actual NVIDIA GPU, CUDA Driver API SDK, compatible driver, and LLVM `llc` with NVPTX):

```sh
cmake -S . -B build-gpu -DBUILD_TESTING=ON -DATTO_GPU_G2_ENABLE_PHYSICAL_CUDA_TESTS=ON
cmake --build build-gpu --target oreslang-gpu-g1-ir oreslang-gpu-g2-cuda-hardware-test
ctest --test-dir build-gpu -R '^native_atto_gpu_g1_offline_ptx_no_device$' --output-on-failure
ctest --test-dir build-gpu -R '^native_atto_gpu_g2_PHYSICAL_CUDA_SAXPY$' --output-on-failure
```

No hardware case can run by default: CMake explicitly keeps physical tests OFF, refuses to configure if CUDA/NVPTX prerequisites are absent, and uses a GPU resource lock. The offline PTX CTest must succeed before the physical test is eligible.

### Proof/review blockers

1. **Run real executable tests against the exact PR head and exact Git tree.** The GitHub runner is currently failing jobs at `steps=null`; this is runner admission failure, never compiler or hardware proof.
2. Validate the same provider sources against the repository's **real** `GpuBufferFence.hpp` under GCC/Clang, sanitizers and failure injection on an admitted runner. Local standalone compilation may use mock-only interface stand-ins; those runs are not full-repository evidence.
3. Physical CUDA G2: verify device name/PCI identity and driver version; fingerprint source `.atto`, LLVM IR, PTX, compiler version, binary and executed steps, plus numerical output for every workload and safe cancellation.
4. Security review: authentic source-to-kernel ABI hash, signed capability/tenant context, device generation/fence binding, pinned host staging and no actor-carrier blocking; persistent supervisor-owned quarantine on device loss; driver error handling; hardware OOM/quota faults.
5. Integration: replace the closed-shape G1 compiler with verified `.atto` typed GPU HIR/MIR and ownership/effect checker, then prove portable Java/LLVM/.NET/Wasm/Forge behavior. No implicit CPU fallback or runtime hand-waving.

## Local evidence only

The exact GitHub source blobs were read back and compared to matching local files. A local independent g++ host mock test passed **112 assertions**, including an AddressSanitizer/UBSan run. The real CUDA Driver API header was checked for C++17 signatures using a *local temporary header stand-in*, not a downloaded NVIDIA CUDA SDK; no physical device or actual CUDA header was available. These are host tests, **not hardware execution**. All G2 physical GPU outcomes remain `NOT_EXECUTED`.
