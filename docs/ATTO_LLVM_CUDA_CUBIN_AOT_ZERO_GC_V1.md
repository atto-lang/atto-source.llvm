# LLVM native GPU AOT and zero-GC contract (first executable host slice)

**Status: G0/G1 provider bootstrap; no compiled AttoLang device kernels or physical GPU execution claimed.** This is a C++ native runtime primitive, **not** an alternate guest language and not a substitute for source-to-device lowering. New guest-visible .atto code and full guest tests remain mandatory.

## Non-negotiable invariants

1. **LLVM guest program fully AOT:** the real AttoLang frontend must produce linked native machine code before the process starts. Never interpret/JIT guest bytecode, synthesize GPU code from strings, use NVRTC, or call OpenCL clBuildProgram from a strict-AOT GPU profile. The legacy OpenCL provider is NOT strict device AOT; do not silently route this profile to that provider.
2. **Device code fully AOT:** accept finalized NVIDIA CUBIN ELF64 only. Refuse PTX text, CUDA fatbin containers that might select PTX, LLVM bitcode, incomplete relocatable CUDA ELF, unknown ISA, and target SM mismatches. No automatic CPU fallback. A matching precompiled SM is required. Initial admission supports only SM 70/72/75/80/86/87/89/90, deliberately excluding SM 100+ until driver off-target finalization/reproducibility are separately analyzed. The device driver is still the ultimate binary validator.
3. **Zero tracing GC:** AttoLang actor memory is owned by per-actor allocation domains, with explicit move/borrow/share/retirement. The C++ CUDA host shim owns fixed-size opaque handles and does not employ a garbage collector. Zero GC is not zero allocations: driver implementations may allocate resources. The shim does not prove the entire LLVM language runtime is zero-GC; verify every runtime path separately.
4. **Nonparking async:** CUDA launch records an event on a supplied stream and returns immediately after enqueue. Poll CUDA_ERROR_NOT_READY (600) without waiting on the carrier. Cancellation is only a request. Unknown driver errors are not completion proof. Kernel/module destruction is forbidden while an event is pending/unknown, and a destructor in this state terminates instead of silently releasing potentially live device code.
5. **Tenancy and ownership:** SystemDriver demands an existing current CUDA context; it does not create one or authenticate the caller. A trusted tenant broker must own the context and stream, query real device limits, authenticate allocation handles, prove buffer loan non-overlap, enforce VRAM/launch quotas, and retain device/actor resources until physical quiescence. The Driver and stream MUST outlive Kernel launches. Raw C++ pointers and synthetic ELF headers are not trusted credentials.

## What this PR adds

- CudaCubinAot.hpp: strict bounded CUBIN preflight, exact-SM checks, checked logical launch products and per-block quotas, allocation-free host-side Kernel launch state, explicit cancellation and event-poll retirement.
- CudaSystemDriver.hpp: opt-in native CUDA Driver API adapter dynamically loading libcuda.so.1 or nvcuda.dll. Uses cuModuleLoadData and cuLaunchKernel, not CUDA compilation/linker APIs, and operates only on a pre-existing current context. No CUDA SDK needed to compile the adapter. This is the provider API path to a real GPU, **not** a successful GPU run.
- cuda_cubin_aot_test.cpp: fake-driver negative cases for PTX/binary mismatch, SM mismatch, launch overflow/limits, reentrant submission, cancellation and unknown-event fencing.
- cuda_cubin_system_smoke_test.cpp: dynamic driver/context discovery. Returns code 77 and reports NOT_EXECUTED when no trusted CUDA context; CTest treats it as SKIPPED, never as hardware pass.
- CMakeLists.txt: opt-in host tests under normal BUILD_TESTING, with strict C++17 warnings.

The test's 64-byte synthetic ELF header is *only a header-shape fixture*. It is NOT a real loadable CUBIN, and Mock driver acceptance is not GPU execution.

## Local host build without CUDA toolkit

From the source repository root:

~~~sh
g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -UNDEBUG \
    -I src/main/cpp tests/cuda_cubin_aot_test.cpp -o /tmp/atto-gpu-aot-test
/tmp/atto-gpu-aot-test

g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror \
    -I src/main/cpp tests/cuda_cubin_system_smoke_test.cpp \
    -ldl -o /tmp/atto-gpu-context-test
/tmp/atto-gpu-context-test  # exit 77 = NOT_EXECUTED without trusted CUDA context
~~~

Host tests are independently compiled under GCC 14 and Clang 17, including ASan+UBSan. Do not promote source guest/GPU claims from these tests. The full repository CMake/CTest and existing LLVM backend suites still need exact-head execution.

## Real GPU SAXPY promotion gate (next implementation PR)

1. Implement typed .atto kernel declarations, restricted device effects, and generation/ownership-aware GPU views in the real LLVM Parser, OwnershipChecker and LLVM Codegen. Emit a deterministic target-specific device artifact via the offline NVPTX/PTXAS toolchain. A manually written .cu kernel may bootstrap the driver test but **cannot** be credited as AttoLang kernel compilation.
2. Build offline for an exact GPU target; package a *real*, immutable, finalized .cubin plus offline toolchain/output digests. The default strict-AOT profile must not include PTX fallback, runtime source compilation, or a JIT-capable alternate backend.
3. In a trusted test GPU context, bind a tenant-authorized CUDA stream, provider-owned device buffers and authenticated quotas; upload f32 input, launch SAXPY with bounds checks including a non-multiple-of-256 tail, poll the event without parking, download the output, and compare against independent scalar reference.
4. Execute device-loss, stale generation, wrong-tenant, overlapping mutable loans, cancellation-before-completion, unload-before-quiescence and exact-SM mismatch negatives. Actor panic/destruction must retain a GPU operation until the physical fence drains.
5. Publish exact .atto source/blob, compiler SHA, host executable hash, CUBIN hash/target architecture, verified device identity, executed test steps, oracle results and artifact binding. A CTest skip (77), CPU OpenCL ICD, or host mock has **no** physical GPU parity.

## Backend scope

This strict CUDA-CUBIN path targets the native LLVM backend. Other backends may use the same AttoLang surface semantics with *separately proven* full AOT where required and appropriate physical GPU backends (e.g. Vulkan/SPIR-V/WebGPU with explicit capability negotiation). Do not assert Java, .NET, Wasm or Forge GPU parity from this code.

Related: platform-conformance PR #505, Java #2575, LLVM #1137 and LLVM #1166.
