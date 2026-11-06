# GPU-G1 source-to-NVVM IR bootstrap (stacked prototype)

**Host-only compiler validation. Not PTX generation, GPU execution or general `gpu fnc` compiler support.**

`oreslang-gpu-g1-ir <kernel.atto>` accepts one explicitly bounded canonical SAXPY kernel and emits NVVM-compatible LLVM IR to stdout. The accepted syntax is restricted by a token-level closed shape: `pub gpu fnc saxpy(f32 scale, GpuArray<f32> x, GpuArray<f32> y, GpuArray<f32> mut out, u64 n) => void`, then `gpu.global_id(0)`, `if i >= n { return; }`, and `out[i] = scale * x[i] + y[i]`. The compiler rejects variations in parameter types, effects, memory indexing or arithmetic. This is a **compiler bootstrap**, not a general language feature.

The generated kernel has NVPTX target triple, `ptx_kernel` calling convention, NVVM thread intrinsics, 64-bit global-index computation, an unsigned `i < n` guard, and explicit global-address-space F32 loads/stores. It is suitable for follow-up target-backend validation, provided the target LLVM version accepts opaque pointers and the target NVPTX backend is available. There is no silent CPU fallback.

## Local host checks

```sh
g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror -I src/main/cpp tests/gpu_g1_saxpy_nvvm_test.cpp -o gpu_g1_test
./gpu_g1_test
# Or with the repository's CMake+LLVM build:
ctest --test-dir build -R native_atto_gpu_g1_ --output-on-failure
```

The CLI source is bounded to 64 KiB; no compiler output is emitted on invalid source. Negative source-mutation tests include missing bounds, altered write footprints, host buffer types, asynchronous effects and multiple declarations. Even passing these tests does **not** establish whole-program type checking, buffer ownership, hardware capability admission, cancellation fences or GPU execution.

## Next execution gates

1. Integrate a versioned typed GPU HIR with the ordinary `.atto` frontend, borrow checker and effect checker. The existing native scalar compiler still rejects `launch ... on gpu` with `GPU-LAUNCH-008` and must not silently adopt this isolated prototype.
2. Reproduce **real PTX** offline with LLVM NVPTX (e.g., `llc -march=nvptx64 -mcpu=sm_70`): optional CTest `native_atto_gpu_g1_offline_ptx_no_device` is registered only when CMake finds an NVPTX-capable `llc`. Local validation generated PTX via LLVM 20.1.8 NVPTX backend exposed by `llvmlite` (a transient local driver adapter, **not** committed Python code); source SHA-256 `c39562871d7287634c44180dcf0bc63e6602f7b261f28895eedfed5e29671ec3`, LLVM IR SHA-256 `89203b0f4852dc667bc4357aeab013f13a639d251d71dc3e10871ae4ac0f071c`, PTX SHA-256 `4aae34ebd16d56c0c9838fc0343ce7b4ba760b71847fed8577210a24bfaa5258`. This proves only offline code generation; a normal hosted `llc` lane and PTX entry ABI certification are still pending.
3. Implement an authenticated CUDA Driver API provider with device memory transfers, queue ownership, physical events, quiescence-gated BufferFence retirement and no CPU fallback. Complete independent hardware SAXPY numerical oracle and negative mutation receipts.
4. Pin immutable source/blob/tree/toolchain/driver/device/kernel binary and CI step identities in platform conformance. `steps=null` or `[]` remains runner admission failure, not a compiler or device result.

Review as a stacked draft on LLVM GPU launch source-admission PR #1457; do not merge ahead of borrow/effect/ABI gates.

## Latest local evidence (not hosted CI)

The exact source, frontend header, CLI, and negative test files compiled with `g++ -std=c++17 -Wall -Wextra -Wpedantic -Werror`; 56/56 host compiler source-admission assertions passed. The actual `.atto` file was translated to LLVM IR, verified using LLVM 20, and lowered through **real LLVM NVPTX target emission** to PTX `.visible .entry saxpy` with loads/stores and guarded computation. A local CMake offline-PTX script passed when pointed at an LLVM 20-based adapter. No CUDA device was available; the validation did not call `cuModuleLoad`, `cuLaunchKernel` or collect device outputs. The committed repository remains Python-free.
