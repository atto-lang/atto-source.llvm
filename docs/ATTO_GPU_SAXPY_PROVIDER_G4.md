# AttoLang SAXPY GPU provider G4 — source-derived, three-buffer OpenCL

Status: **implementation committed, execution not yet independently validated**.
This is a deliberately restricted SAXPY f32 vertical slice, not general CUDA
parity, compiler GPU MIR, GPU task/Future lowering or full physical GPU proof.

This branch is stacked on LLVM draft PR #1466, which introduced the closed
`.atto` source-to-OpenCL C compiler mode. Do not merge it independently of
its parent or promote the parent while the required CI and reviews are absent.

## Physical execution path added

1. The opt-in native compiler validates exactly one typed `.atto` SAXPY
   declaration, explicit `i < n` bounds mask, and finite f32 shape.
2. It emits a canonical, fixed OpenCL C kernel with X/Y/output buffers and
   alpha/n by-value arguments. The new `SaxpyProvider` and `SystemApi`
   revalidate canonical artifact identity before any physical resource claim.
3. The trusted adapter selects only an OpenCL `CL_DEVICE_TYPE_GPU` device
   with 64-bit addressing, float32 support and little-endian f32 ABI.
4. The provider allocates **three distinct native GPU buffers** and retains
   copies of both input arrays and the output staging vector for the entire
   asynchronous submission. It uses the system OpenCL adapter to enqueue
   X upload, Y upload, SAXPY kernel and output download, one stage/event at a
   time on the same in-order command queue.
5. Each stage must receive and retain an actual native OpenCL event; polling
   that event to completion permits progression. The provider now counts
   native event completions **after successful event-handle release** and
   refuses a successful SAXPY output without exactly four confirmed events
   (X upload, Y upload, kernel, download). This value is measured, never
   hardcoded into the physical probe. Output is inaccessible until the
   download event completes and all physical releases succeed.
6. A cancel request only marks intention. It does not free native buffers.
   If a native event remains pending, stage state and physical resources stay
   alive. Device loss, failed status queries, null event returns or ambiguous
   enqueue exceptions trigger quarantine. Recovery requires the **actual
   driver's queue-wide `clFinish` proof**, never a guest-provided bool.
7. Quota and generation bookkeeping is provider-owned. An in-flight or
   quarantined operation prevents a second launch on that provider. The
   generation increases monotonically even across failed attempts and refuses
   overflow. Native handles and event pointers do not cross actor/channel
   boundaries. Buffer-alias anomalies are quarantined and physically released
   at most once after trusted queue quiescence.

**Important limits:** These are **per-provider queue and staging quotas**,
not a shared tenant-wide resident-memory budget across independently
constructed providers. The host staging vectors are retained but not
guaranteed page-locked by the OS, so `host_staging_bytes` must not be
reported as physically pinned memory. The provider must remain alive until
its terminal operation is retired; there is not yet a universal shutdown
drain, actor scheduler integration or hot reload operation registry.
The standalone physical probe's small host poll loop is not an actor carrier.
The provider now exposes explicit close() after completion/drain. Closing
while an event or quarantined allocation exists rejects. **Destroying the
provider while active terminates the process** instead of freeing host
staging memory referenced by an asynchronous OpenCL transfer. This is a
deliberate fail-closed emergency invariant, **not an acceptable recovery
strategy** for a production multi-tenant actor host: such hosts require a
supervisor-owned shutdown/drain registry with durable quarantines and
tenant-scoped fault containment before enabling general guest GPU tasks.
The underlying SaxpyApi must also outlive the provider and operations.

## Reproduce build and tests on immutable source

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --target oreslang-llvmc \
  oreslang-atto-gpu-saxpy-source-v1-test \
  oreslang-opencl-saxpy-provider-v1-test \
  oreslang-opencl-saxpy-gpu-probe
ctest --test-dir build --output-on-failure -R 'atto_gpu_saxpy_'
```

`atto_gpu_saxpy_provider_host_v1` is a **fake-driver state machine** test;
its successful result must never be called physical GPU execution.
`atto_gpu_saxpy_physical_opencl_v1` first invokes the native compiler and
passes its exact emitted OpenCL C bytes into the real OpenCL device probe.
If no compatible physical GPU exists, it returns **77 = SKIPPED**, never green
device evidence. The provider and compiler checks can pass on CPU hosts,
but physical results cannot be inferred from them.

On a real GPU worker in an exact, clean, immutable source checkout:

```sh
 # This command itself rebuilds the compiler/probe and runs the host tests
ATTO_EXPECTED_PR_HEAD_SHA="$(git rev-parse HEAD)" \
bash scripts/verify-atto-gpu-saxpy-hardware.sh
```

The strict script checks the canonical tracked fixture blob and archives
the exact Git HEAD into a throwaway source directory. It rebuilds the
compiler/probe from the **committed archive**, excluding ignored/untracked
local headers; caller-provided binaries are not accepted. It runs
native host tests, emits the compiler's OpenCL device artifact, checks it
matches the probe's source-derived kernel, requires a GPU-reported result,
compares all 256 output values bitwise to an independent f32 oracle,
and prints exact head/tree, fixture, artifact, compiler and probe digests.
On hosted CI, supply `ATTO_EXPECTED_PR_HEAD_SHA` from the immutable
`github.event.pull_request.head.sha` **after checking out that exact ref**.
Do not rely on `github.sha`, which may identify a synthetic PR merge commit.
Upload source-tree, toolchain, run ID, run attempt and nonzero executed steps
as separate evidence. Any `steps=[]`/`null` means `NOT_EXECUTED`.
The `CL_DEVICE_TYPE_GPU` flag and a driver-reported name are **not**
cryptographic physical-device attestation: independently validate runner
hardware inventory and the device/driver capability report. All unexpected
OpenCL driver/ICD failures now fail the probe instead of being silently
reclassified as no-compatible-GPU SKIP. The first compatible discovered GPU
is used; additional device enumeration and driver capability evidence remain
necessary before describing a machine as fully surveyed.

## Next release blockers

- Exact-head C++17 compiler/CTest execution, independent review and CI.
  The new narrow host-only ASan/UBSan workflow is *not* GPU execution evidence
  and may still fail before runner allocation; every zero-step result stays
  NOT_EXECUTED.
- Hardware-backed OpenCL run on an independently inventoried GPU, negative mutation tests
  and the exact device-event/fence receipt. This PR alone is not that proof.
- Shared per-tenant/device quotas across multiple provider instances,
  authenticated tenant creation and a real actor-owned three-buffer fence
  claim, complete device UUID/capability attestation, stable host API
  lifetime ownership, supervisor-managed shutdown/recovery (instead of
  process termination), and tested concurrency across queues.
- Connect GPU device operations to nonblocking `gpu task`, typed Futures,
  actor cancellation/termination and generations/hot reload, without
  blocking actor carriers or executing guest callbacks on a driver thread.
- Replace the restricted SAXPY parser with full typed AttoLang GPU MIR,
  borrow/effect checking, safe kernel intrinsics, geometry and CUDA-class
  synchronization functionality. The current source is a *closed slice*.

Related documentation: `ATTO_GPU_SAXPY_SOURCE_V1.md`,
`GPU_PROVIDER_DISPATCH_CONTRACT.md` (historical specification, earlier
provider implementations supersede its initial status), and shared
platform conformance draft #504.
