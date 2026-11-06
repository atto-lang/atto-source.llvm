# AttoLang SAXPY — isolated three-buffer OpenCL provider G2

Status: **proposed source-to-hardware path on a stacked LLVM draft branch**.
Tests and CMake build gates are authored. **No C++ compile, CTest pass, GPU
dispatch, completion receipt or independent numerical hardware proof is
claimed until the exact new PR head is executed.**

## What this wave adds

- `AttoSaxpyProvider.hpp`: an opt-in, trusted **one-operation-at-a-time**
  provider with three distinct opaque physical buffers (X, Y, out), finite
  f32 coefficient, nonzero equal-length input vectors, checked byte sizes,
  program-source generation by the closed `.atto` SAXPY compiler slice,
  copy-owned host staging, per-operation resident/pinned quota and explicit
  validated 64-bit OpenCL artifact.
- `SaxpyHardware` is a distinct hardware capability, NOT a reinterpretation
  of the old two-buffer `OpenClProvider::begin` or legacy DOP kernel ABI.
- `OpenClSystemApi` implements the third-buffer kernel ABI with OpenCL
  `clSetKernelArg` for X, Y, out, alpha and n and a 1D nonzero NDRange.
  Each host-to-device transfer, kernel, device-to-host transfer retains its
  returned native event until `EventStatus::Complete`.
- `tests/opencl_saxpy_three_buffer_test.cpp`: a fake-event reference
  provider that checks independent output math, staging, admission, cancel,
  device-loss quarantine, queue recovery, foreign-operation rejection and
  resource-release failure/retry. These are **host-only**.
- `tests/opencl_saxpy_physical_probe.cpp`: optional GPU-only runtime test
  with direct source-to-OpenCL lowering and bitwise f32 oracle.
  The test returns nonzero if no GPU is available; unlike the older optional
  DOP probe, it cannot report green as a skipped hardware test.
  It runs only when `ATTO_REQUIRE_PHYSICAL_GPU=ON`.

## Trusted state machine

`Reserved -> UploadX -> UploadY -> Kernel -> Download -> Succeeded`.

At every stage only one native event is outstanding. On a pending event,
`poll` does not advance state, free a device allocation, publish output or
block for completion. A completion advances to the next submission.
A cancellation is a request: the current event must be complete, or a
separate trusted shutdown CONTROL path must prove the entire queue
quiescent via `clFinish`. Uncertain event status, device loss, or an
exception/unknown enqueue result quarantines the entire operation. The
provider retains physical resources until such proof. Failed physical
release is retryable, with the queue admission blocked throughout.

**No actor-carrier code may invoke** `prepare`, `poll`,
`recover_after_quiescence` or `shutdown_after_quiescence` directly.
These functions must be placed on a trusted GPU control/reactor worker;
`prepare` may build a program and the explicit recovery path may call
blocking `clFinish`. A future actor interface should only receive an
owned Future and a CONTROL-delivered completion. No actor integration exists
in this wave.

The provider destructor terminates rather than permitting staging to be
freed while a GPU command might still DMA to it. Hosts must drain or prove
quiescence before destroying an active provider.

## Run the native gates

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --target oreslang-llvmc \
  oreslang-atto-gpu-saxpy-source-v1-test \
  oreslang-atto-saxpy-three-buffer-test \
  oreslang-atto-saxpy-physical-probe
ctest --test-dir build --output-on-failure -R 'atto_gpu_saxpy_'
```

The commands above do not enable the physical test by default.

On a controlled machine with a **real OpenCL GPU**, a privileged operator may
instead use:

```sh
cmake -S . -B build -DBUILD_TESTING=ON -DATTO_REQUIRE_PHYSICAL_GPU=ON
cmake --build build --target oreslang-llvmc \
  oreslang-atto-saxpy-three-buffer-test \
  oreslang-atto-saxpy-physical-probe
ctest --test-dir build --output-on-failure -R 'atto_gpu_saxpy_'
```

Require a receipt containing the exact PR head/tree and source hashes,
native compiler binary hash, device/vendor/driver capabilities, nonzero
executed steps, numerical oracle outcome, GPU event completion and
release accounting. Without that data record `NOT_EXECUTED`.
A CTest exit code alone does not constitute independent physical attestation.

## Still blocking release

- **Tenant/isolation:** the tenant label and queue generation are validated
  nonempty/nonzero but not backed by a global authenticated tenant registry
  or a driver-level multi-provider lease. The single-operation lane forbids
  resource sharing *inside this provider instance* only. **Do not expose it
  to untrusted guest or multi-tenant callers.**
- **Generational fencing:** externally visible reusable DeviceBuffer handles,
  claim/revoke generation advancement and three-way atomic buffer ownership
  must be added before true reusable actor-facing GPU buffers.
- **Resource quotas:** per-operation reservation is bounded, not yet a
  system-wide per-tenant GPU budget shared with the old DOP provider.
- **Future/actors:** no `gpu task` -> Future lowering, carrier-safe wake,
  structured cancellation/supervision, or hot-reload generation fencing yet.
- **Launch geometry:** current backend uses a single 1D global dimension
  derived from n; explicit grid/block/shared-memory provider admission,
  physical occupancy limits and advanced CUDA operations remain future work.
- **Physical execution:** default CI without a compatible GPU does not test
  the physical device path; only opt-in required-GPU lane may prove it.
- **Source integration:** the hardware probe parses the same restricted
  .atto source grammar in the shared compiler header; a general AttoLang
  program cannot yet call the kernel through ordinary MIR/codegen.

This is intentionally a stacked PR on LLVM #1466. It does not merge or
change the existing Java GPU, G3 DOP provider or platform-conformance PRs.
