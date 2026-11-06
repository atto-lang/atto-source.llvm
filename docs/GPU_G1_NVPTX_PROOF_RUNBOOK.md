# GPU-G1 NVPTX proof runbook

**Status: experimental typed device IR. NOT compiled AttoLang guest source.** Exact PR head, clean tree, LLVM IR and PTX SHA-256, driver/device metadata, executed runner steps and numerical/redzone results must be independently observed before reporting physical GPU execution.

## Stage A — typed IR and real LLVM parser

The closed G1 backend emitter at src/main/cpp/oreslang/backend/GpuG1NvptxSaxpyIr.hpp emits an NVPTX-targeted ptx_kernel LLVM module. It uses NVVM block/thread registers, unsigned 64-bit indexing, explicit index bounds, addrspace(1) device f32 buffers, and ordered multiply/add/store. It rejects host buffers, unsafe effects, arbitrary captures and CPU fallback. **It does not parse gpu fnc from an .atto file.**

Run native_gpu_g1_typed_nvptx_saxpy_ir_host_only and, where Clang is available, native_gpu_g1_llvm_ir_parse_and_mutation_host_only. The latter requires a deliberately broken SSA mutation to fail with no output artifact. Passing here means HOST_ONLY and LLVM_IR_ONLY, never physical GPU parity.

## Stage B — actual PTX emission

Configure with -DATTO_GPU_G1_REQUIRE_NVPTX=ON so missing llc is a configuration failure. Run native_gpu_g1_llc_ptx_no_device_claim. This executes a real llc NVPTX backend over the typed LLVM IR, checks the PTX entry and 64-bit addressing, and prints SHA-256 for the IR and PTX artifacts. It removes a partial PTX output on failure. A missing NVPTX backend must fail; **PTX output is not GPU execution**.

## Stage C — physical NVIDIA proof (strictly opt-in)

Use an isolated, reviewed NVIDIA GPU machine with CUDA Driver API development files, a real GPU driver and an NVPTX-capable llc. **Do not execute untrusted PR code automatically on a privileged self-hosted runner.** Obtain the expected PR head SHA from independent GitHub metadata, not by substituting the checkout's own HEAD.

After checking out that exact head in a clean source worktree, configure and run:

    cmake -S . -B /tmp/atto-gpu-g1-build -DBUILD_TESTING=ON \
      -DATTO_GPU_G1_REQUIRE_NVPTX=ON \
      -DATTO_GPU_G1_REQUIRE_PHYSICAL=ON \
      -DATTO_GPU_G1_EXACT_HEAD_SHA=<independently-verified-40-hex-PR-head>
    cmake --build /tmp/atto-gpu-g1-build \
      --target oreslang-gpu-g1-typed-saxpy-ir-test oreslang-gpu-g1-cuda-physical-probe
    ctest --test-dir /tmp/atto-gpu-g1-build \
      -R '^native_gpu_g1_' --output-on-failure

The physical gate rejects wrong head, dirty tracked worktree, missing PTX, absent device, low capability, driver errors, incorrect SAXPY results or overwritten output guard elements. It checks six logical lengths: 1, 255, 256, 257, 1025 and 65537. Its blocking cuCtxSynchronize() is TEST ONLY; production Atto Future completion must suspend tasks without blocking carrier threads or reclaiming in-flight buffers.

## Stage D — source compiler bridge still missing

Do not classify a Stage C pass as ATTO_SOURCE_COMPILER or cross-backend parity. Draft LLVM source-admission PR #1457 currently rejects positive GPU launch execution, while Java draft #2582 includes parser/admission groundwork. The next step is to lower verified .atto gpu fnc declarations and launch AST through borrow/effect checking and canonical GPU HIR/MIR, then invoke this backend. Prove ownership, device/queue generation fencing, cancellation, tenant isolation and scheduler-safe completion separately.

Companions: LLVM launch parser #1457, Java frontend #2582, common source #245, conformance #502 and this independent GPU-G1 device slice #1473. Keep all drafts until exact-head nonzero-step hosted proof and independent review.
