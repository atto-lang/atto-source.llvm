# AttoLang GPU G1 — independently pinned PTX module and CUDA kernel bridge

**Evidence class:** HOST_ONLY_HASH_GATE + CUDA_SDK_COMPILE_OPT_IN. This work does not execute a GPU device, authenticate the compiler, or establish general AttoLang GPU source support.

## Stage and trust boundaries

The restricted compiler work in LLVM #1473 accepts one approved `.atto` SAXPY body, generates LLVM IR and can lower it to PTX offline. `GpuG1VerifiedPtx.hpp` adds an OpenSSL EVP-SHA256 byte gate before a CUDA module may be loaded. `GpuG1CudaModuleLoader.hpp` uses the CUDA Driver API only after this gate has passed; `GpuG1CudaDriver` requires an approved module `KernelToken` and **cannot be constructed from an arbitrary raw `CUfunction`**.

The `PinnedG1Ptx` policy fields are: exact 40-hex PR head Git SHA, tree Git SHA, fixture Git blob SHA, 64-hex compiler binary SHA-256, expected PTX SHA-256, ABI version 1 and the sole entrypoint `atto_g1_saxpy_f32`. The SHA fields are validated for lowercase hexadecimal shape. The loader computes the *actual* PTX digest with OpenSSL SHA-256 and compares bytes in constant time. PTX is restricted to a 1 MiB text envelope without interior NUL. The hash comparison happens **before** calling `cuModuleLoadData`. Only the fixed entrypoint is accepted by `cuModuleGetFunction`.

**CRITICAL:** Neither a matching digest nor matching SHA *strings* authenticates the origin of those expected pins. The expected manifest MUST be obtained by an independently authenticated pipeline from the exact PR head, immutable fixture/source identity, compiler executable and NVPTX emitted artifact. An adversary who chooses both PTX bytes and expected hash can trivially satisfy this structural gate. A hardcoded placeholder SHA must never be accepted as production provenance. Integrate trusted manifest verification/signatures before any physical GPU capability claim.

## Module ownership

The `GpuG1VerifiedPtxModule` issues a token bound to both the kernel and the host module loader incarnation. The CUDA driver constructor checks the module loader's CUDA context against its own context, holds the token for its lifetime and rejects raw kernel handle construction. `close_after_quiescence(provider)` refuses to unload while the event provider has any outstanding queue, allocation or event; it also refuses to unload while any driver/kernel token exists.

Kernel-token minting and quiescence-gated unload share one module mutex so an in-flight token creation cannot race with a module unload decision. Only the host module loader can mint a token from an independently pinned PTX image.

The module intentionally has no automatic destructor-driven `cuModuleUnload` when the host violates teardown ordering. A GPU code-resource leak is safer than unloading code potentially executing on the device. A failed module load/resolve attempts cleanup, but when CUDA cleanup fails the owner must quarantine and retire the CUDA context under an independently governed recovery protocol. The host must also keep `GpuG1CudaModuleLoader` alive until the module is closed and all kernel tokens are dropped.

## Actual GPU and transfers remain incomplete

- `GpuG1EventProvider` still has a restricted SAXPY launch, cancellation, and event poll bridge, but **does not yet implement bounded H2D/D2H data-transfer futures or pinned host-staging lifetimes**.
- Do not hand a raw CUDA stream, allocation pointer or unverified `.atto` kernel to the guest as a capability; transfers must use provider-owned allocation generations and queue streams, hold a read/write loan until a same-stream CUDA event completes, and retain pinned host staging until then.
- Module lifetime must cover all uploads, launches, downloads and final events. No CPU fallback, no implicit ownership escape, no fake event completion and no host-carrier blocking.
- CUDA real SDK compilation and physical device execution remain **NOT_EXECUTED** unless a genuine installed CUDA SDK and authorized NVIDIA hardware prove them.

## Tests / gates

- `gpu_g1_verified_ptx_test.cpp`: 1,069 executed local host-only assertions including 1,024 deliberate PTX byte mutations, malformed SHA/entrypoint/ABI, oversized and embedded-NUL PTX, and module pin/unload/resolve failures. GCC and Clang strict C++17 and ASan/UBSan locally cover this separate hash gate.
- `gpu_g1_cuda_adapter_compile_test.cpp` has a compile-time negative assertion that a raw `CUfunction` constructor does not exist, and a positive assertion for an approved kernel token.
- Optional `ATTO_GPU_G1_CHECK_CUDA_SDK=ON` adds SDK compile/link tests for the CUDA module loader and driver. A locally supplied **test-only stub** `cuda.h` may establish C++ API syntax, never real CUDA ABI compatibility or hardware evidence. Do not ship the stub.
- The normal exact-head host CI compiles/runs the hash suite using `libcrypto` and checks Git source blob identity. A hosted run with `steps=null` or `[]` is a runner-admission failure, not executed proof.
- Independent reviewer must validate SHA policy origins, source compiler isolation, CUDA context/module lifetime, error rollback, tenant device ownership and execution receipt collector (#549), then inspect exact-head executed CI and physical SAXPY numeric/redzone oracle.

**Stack:** LLVM #1473 (restricted `.atto` compiler) → #1506 (geometry and buffer quotas) → #1508 (loan registry) → #1512 (CUDA provider) → this draft verified-module bridge.
