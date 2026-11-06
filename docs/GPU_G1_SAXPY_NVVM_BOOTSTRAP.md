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
2. Lower this exact emitted IR to **real PTX** with an installed LLVM NVPTX backend (e.g., `llc -march=nvptx64 -mcpu=sm_70` after LLVM IR verification), record input/output hashes, verify the entry ABI. The local environment used for this prototype had neither `llc` nor an NVIDIA GPU.
3. Implement an authenticated CUDA Driver API provider with device memory transfers, queue ownership, physical events, quiescence-gated BufferFence retirement and no CPU fallback. Complete independent hardware SAXPY numerical oracle and negative mutation receipts.
4. Pin immutable source/blob/tree/toolchain/driver/device/kernel binary and CI step identities in platform conformance. `steps=null` or `[]` remains runner admission failure, not a compiler or device result.

Review as a stacked draft on LLVM GPU launch source-admission PR #1457; do not merge ahead of borrow/effect/ABI gates.
