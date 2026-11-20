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

## Security audit and hardening update (2026-11-06)

The G1 audit adds `GpuG1SaxpyLaunchAdmission.hpp` and `gpu_g1_saxpy_launch_admission_test.cpp`. Structural preflight verifies nonzero tenant/device/queue and allocation generations, view bounds and f32 memory budget, exclusive output footprints against two read inputs, checked group/grid multiplication, and zero logical extent. The physical CUDA *test harness* now builds this preflight from real CUDA allocation addresses and device-reported group/grid limits. The tenant, queue and generation IDs in the harness are **fixtures**, not authenticated provider capabilities; an actual AttoLang provider must supply trusted identities and atomically claim loans through device quiescence.

The exact-head receipt script also revalidates Git head, source tree and clean tracked/untracked worktree after PTX generation and after the CUDA probe. It records and rechecks SHA-256 for the Git tool, emitter, `llc`, CUDA probe, PTX gate, receipt gate, LLVM IR and PTX. Any mutation is rejected. This is provenance hardening, **not an assertion that the hardware runner itself is trusted**; separate independent runner attestation/review is required for physical certification.

The source-free `gpu_g1_receipt_toctou_negative.cmake` exercises four adversarial cases: source modification by PTX gate, source modification by a fake successful device probe, PTX bytes changed after hashing, and self-modifying PTX gate. Each must fail without device-evidence promotion. It is registered only on UNIX hosts with Git and is explicitly HOST_ONLY.

**Local executed evidence on exact committed source/test Git blobs:** C++17 27/27 closed device IR assertions, 29/29 structural admission assertions, actual Clang LLVM-IR parsing and rejected undefined-SSA mutation with no artifact, and all four TOCTOU negative scenarios. These passed as four separate tests using a standalone local CMake/CTest harness; the repository-wide LLVM build and hosted CI have **not** executed on this revised PR head. CUDA test compilation used fake header declarations and a zero-device fake driver *only* for negative admission; no NVIDIA GPU or NVPTX `llc` was available locally.

Hard blockers remain `ATTO_SOURCE_TO_IR=NOT_IMPLEMENTED`, `NVPTX_PTX=NOT_EXECUTED`, `PHYSICAL_GPU=NOT_EXECUTED`, scheduler-owned async `Future<void>` and real authenticated allocation/queue/fence ownership. Do not merge until these gates and independent review are satisfied.

## Restricted .atto source compiler bridge (2026-11-06)

A new **opt-in** kernel-only compiler gate admits the one canonical SAXPY source fixture in `tests/fixtures/gpu-g1-saxpy.atto`. Run the normal native compiler as:

    oreslang-llvmc --gpu-g1-kernel-ir tests/fixtures/gpu-g1-saxpy.atto

The isolated `oreslang-gpu-g1-atto-saxpy-llvmc` binary exercises the same bounded lexer and verified G1 device-IR emitter, independently of the native compiler's larger LLVM dependency graph. Source bytes must be a regular, pinned UTF-8 .atto input within 64 KiB in the native mode; any additional compiler flags, kernel/host declarations, unresolved device calls, changed bounds, forbidden effects or extra source are rejected before IR output. The positive set is **exactly one recognized SAXPY body** with an optional terminal `return;`, not a general GPU parser, typechecker, host launch implementation or arbitrary user function codegen.

The PTX gate now accepts `G1_SOURCE` and `G1_SOURCE_FLAG`, and the opt-in physical gate requires both a Git-tracked .atto source fixture and the main native compiler flag `--gpu-g1-kernel-ir`. The physical receipt pins the source SHA-256. The gate cannot replace compiler source input with the older preconstructed device IR while still claiming source evidence.

**Executed local host verification:** exact-file G++/C++17 restricted source compiler tests **50/50**, an isolated CMake/CTest run **2/2**, complete .atto fixture -> canonical G1 LLVM IR equivalence -> real Clang LLVM IR parser, invalid bounds and appended host-code rejection with no IR stdout. The GitHub blobs of the source admission header, CLI, tests and fixture were checked against the local files. The main `oreslang-llvmc` build is still NOT_EXECUTED in hosted CI because runner admission is failing.

**Offline PTX experiment (not the hosted llc gate or GPU):** LLVM 20.1.8 NVPTX via a local llvmlite binding emitted PTX for the real restricted .atto fixture. The PTX contains a named kernel, 64-bit addressing, a bounds branch, global F32 loads/store, and round-to-nearest F32 multiply/add. Source SHA-256 `0efd5ee7e90765d82d4059324413387b63c3559a9334815072277b3ae75c9482`; LLVM IR SHA-256 `fab2215fe3bc072989733f4e10b2bd2183ee99b9509ef8575e4eded738dbe314`; offline PTX SHA-256 `a1183dbc981e7be66dfad48b5dadb160ca6b6ccf4adf4434230bb4cf29871e35`. This is a **local codegen proof**, not an independently executed GitHub exact-head build or physical GPU evidence.

Remaining steps: executable exact-head hosted CTests/JUnit with nonzero job steps, actual repository `llc` NVPTX test, CUDA hardware numerical/redzone proof through the main compiler, verified `.atto` host `launch` parsing/ownership + asynchronous scheduler-owned device completion, and independent review. Remain draft.

## G1.1 bounded typed source parser and exact-head CI follow-up (2026-11-06)

The earlier immutable literal-token table has been replaced by a bounded `SaxpySourceParser` that produces a typed `SaxpySourceAst` with five parameter roles (f32 scalar, two read-only f32 GPU buffers, exclusive writable f32 output, u64 extent) and an immutable `gpu.global_id(0)` lane. Parameter and lane identifiers can be **consistently alpha-renamed**. The parser checks binding identity for the guard and each indexed read/write, rejects duplicate/shadowing/reserved bindings, and permits only the proven `out[i] = scale * x[i] + y[i]` dataflow and an optional final return. Arbitrary expressions, dynamic axes, host calls, actor/await effects and host launch remain rejected. This is still a narrow kernel-only parser, **not** a general GPU AST/HIR/MIR or borrow-checked device provider.

`tests/gpu_g1_typed_frontend_matrix_test.cpp` adds **12 positive and 71 negative source mutations**; its CTest is `native_gpu_g1_typed_frontend_alpha_rename_negative_host_only`. Local clang++ C++17, ASan and UBSan execution passed **83/83 test cases**, and the test's exact GitHub blob matched the locally compiled file. The updated parser's source logic was exercised locally; the full repository compiler still requires hosted exact-head execution.

The new dedicated `.github/workflows/gpu-g1-exact-head-compiler.yml` contains separate unprivileged Ubuntu jobs for bounded source admission and full native `oreslang-llvmc` → LLVM 18 `llc` PTX emission. Both check exact PR head/tree and do **not** claim physical GPU evidence. Run 42328030098 reported **failure with steps=null for both jobs**: `RUNNER_ADMISSION_FAILURE`, not a source or PTX test result. Administrative runner/billing restoration is tracked in LLVM issue #159, with no private-source runner transfer authorized.

**Reproduced offline, non-CI evidence:** the canonical G1 .atto fixture emitted LLVM IR whose SHA-256 is `fab2215fe3bc072989733f4e10b2bd2183ee99b9509ef8575e4eded738dbe314`; Clang 17 parsed it, and LLVM 20.1.8 NVPTX via llvmlite produced PTX SHA-256 `0c0fd14fcbada62f81b56eb1bd2fa8503248144a4652fba94e914722f2824f86`, containing the kernel entry, `.address_size 64`, global F32 loads/store and round-to-nearest multiply/add. This codegen did **not** execute a hardware kernel. The PTX hash can vary by LLVM version/target options and is not a cross-version golden expected output.

Conformance companion #502 contains five new kernel-only source-shape fixtures with immutable Git blob ledger, all remaining NOT_EXECUTED on hosted compiler/guest/device lanes. Do not promote or merge without independent reviewer approval and genuinely executed exact-head compiler and NVIDIA hardware receipts.
