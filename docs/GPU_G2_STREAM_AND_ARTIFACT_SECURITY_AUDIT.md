# GPU-G2 audit: stream ownership and source-to-PTX trust boundary

**2026-11-06. Status: draft stacked security patch. Physical NVIDIA execution: NOT_EXECUTED.** Parent: [GPU-G2 CUDA provider #1486](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/1486), stacked on [source-to-NVVM compiler #1469](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/1469). This audit intentionally does **not** modify their heads or claim GPU hardware parity.

## Findings and changes

| Priority | Finding | Reproduction / mitigation |
| --- | --- | --- |
| HIGH | `SaxpyOperation` was single-flight **per object**, but distinct objects sharing the same `Driver` could enqueue into its one CUDA stream concurrently, invalidating its assumed single-operation event/staging order. | Added mutex-protected driver-wide exclusive stream lease; a contender rejects `GPU-G2-005` with no device handle allocation; lease is released only **after** all buffers/events/modules and the three authenticated `GpuBufferFence` owners are retired. Quarantine, cancellation and cleanup failure hold the lease. |
| HIGH | CTest `DEPENDS native_atto_gpu_g1_offline_ptx_no_device` does not itself force a separately filtered producer test to run. Selecting just the physical test could read a **stale pre-existing PTX** file. | Changed the physical test to invoke `atto_gpu_g2_physical_chain.cmake` which checks the canonical `.atto` SHA-256 (`c39562871d7287634c44180dcf0bc63e6602f7b261f28895eedfed5e29671ec3`), deletes old IR/PTX, and regenerates source → LLVM IR → PTX in the **same** physical-test invocation before the GPU executable. No CPU fallback. |
| HIGH | The hardware test read an arbitrary-length PTX file into `std::string` before the provider's 512 KiB limit, enabling a runner OOM via malformed/untrusted files. | New `G2PtxInput.hpp` uses a fixed-size streaming read with a 512 KiB bound, rejects empty/bad streams, embedded NUL and missing SAXPY entry **before CUDA driver initialization**. Eight standalone host-only input tests cover boundary lengths and negative cases. |
| MEDIUM | A mutated source fixture could fail provenance validation while leaving a prior PTX artifact on disk. | Clear old `kernel.ll` and `kernel.ptx` **before** source digest admission. Two always-registered source-failure/target-failure CMake negative tests verify that old PTX cannot survive. |

## Local executed evidence — not CUDA hardware

- **307,078 host-only fake-driver assertions passed** using locally compiled exact-source `G1CudaProvider.hpp` and `gpu_g2_cuda_provider_host_test.cpp`. Coverage includes overlapping submissions across distinct tenants, mutable shared-stream serialization, 128 four-way concurrent stream admission races (512 attempted submissions), 300 seeded SAXPY CPU-oracle vector/geometry trials, allocation failures at all three positions, upload/launch/download/event failures, quarantine, cancellation, event/module cleanup retries, geometry limits and exact operation state.
- **8 host-only bounded PTX input assertions passed**, including the 512 KiB boundary, 512 KiB+1 rejection, malformed input and embedded NUL.
- **2 host-only CMake mutation scenarios passed**: mutated source and failing LLVM target both rejected without retaining any stale PTX.
- The updated **307,078-assertion** fake-driver suite passed under both GCC and Clang with AddressSanitizer/UBSan, using locally compiled repository-matching source blobs and **stand-in** runtime headers. The bounded-input host test also passed under sanitizers.
- The CMake pipeline orchestrator previously generated real offline NVPTX for the pinned `.atto` fixture with LLVM 20.1.8 (via temporary local `llvmlite` adapter); its final **hardware invocation was synthetic** and is **not physical GPU evidence**. The hardware test has since expanded to **75 planned cases**: eight fixed boundary sizes, 64 seeded numerical/geometry workloads, two fail-closed geometry/quota admissions, and one cancellation case. The updated hardware suite compiled in syntax-only mode under GCC and Clang against an interface stand-in; **the new 75-case hardware suite has NOT_EXECUTED**.
- Exact Git blob identities verified by GitHub readback and matching local `git hash-object` for all seven new/modified standalone provider/test/script files.
- These tests used local **stand-in** `GpuBufferFence.hpp`, `GpuLaunchGeometry.hpp` and CUDA header signatures; no full-repository CMake/CTest or real SDK/device runs have completed.

### Remaining blockers — do not promote

1. Physical NVIDIA device, authentic CUDA Driver API headers/library, actual `cuLaunchKernel`, hardware output comparison and 75 real hardware cases; all remain `NOT_EXECUTED`.
2. Exact-head hosted CI with nonzero executed steps. A workflow failure with `steps=null` is **runner admission**, never compiler/test evidence. Independent review required before merging.
3. Replace `.entry saxpy` substring checks with **authenticated, compiler-issued PTX provenance and kernel ABI attestation** before exposing any device compilation/dispatch to untrusted guest code.
4. The CUDA adapter uses pageable host staging, which may block at driver submission; this is **not** the AttoLang async actor/scheduler API. Production requires pinned host memory, bounded async queue admission and no carrier blocking.
5. Process-fatal destructor for unretired handles is fail-stop rather than durable supervised quarantine; replace with runtime-owned retention/recovery after proven device quiescence.
6. Validate G2 against the actual upstream `GpuBufferFence.hpp` and `GpuLaunchGeometry.hpp`, with exact-head build/test artifacts and sanitizer proofs; local interface stand-ins do not certify those implementations.
7. No general typed `.atto` GPU HIR/MIR, kernel borrow/effect proof, multi-tenant capability broker, stable hot-reload generation fencing or Java/LLVM/.NET/Wasm/Forge parity yet.

**Merge policy:** keep stacked PR draft. Require independent approval, source/tree/blob identity and authentic nonzero-step CI plus physical GPU and negative-mutation evidence for any hardware-verified claim.
