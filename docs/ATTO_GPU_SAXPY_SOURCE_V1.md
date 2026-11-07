# AttoLang SAXPY GPU source slice v1 (experimental, non-dispatch)

This is a **real, narrowly restricted source-to-OpenCL C compiler mode**, not
a production GPU runtime or proof of CUDA feature parity. It is separate from
the normal scalar LLVM frontend, which continues to fail closed for the new
GPU grammar. The public language name is **AttoLang** and the input extension
is `.atto`; legacy internal Oreslang symbols and repository names are not
changed by this slice.

## Compile the guest kernel into a device-source artifact

```sh
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --target oreslang-llvmc oreslang-atto-gpu-saxpy-source-v1-test
build/oreslang-llvmc --emit-opencl-gpu-v1 tests/fixtures/atto-gpu-saxpy-f32.atto > /tmp/atto-saxpy.cl
ctest --test-dir build --output-on-failure -R 'atto_gpu_saxpy_'
```

**The compiler flag only emits OpenCL C; it does not build an executable,
allocate memory, run a physical GPU, schedule a Future, or supply a device
completion event.** The normal compiler invocation with this .atto source
must fail. No implicit CPU fallback is permitted.

## Admitted semantics

The source contract is an exact closed kernel AST shape:
`compute fnc saxpy(x: DeviceRead<f32>, y: DeviceRead<f32>,
out: DeviceWrite<f32>, alpha: f32, n: u64) device`, with
`const i = gpu.global_id.x;`, an explicit `if i < n` mask, and
`out[i] = alpha * x[i] + y[i];`. Whitespace and ordinary comments are
allowed. **Other functions, declarations, host effects, pointer constructs,
arithmetic variants, device calls, nested regions, and launch statements
are rejected rather than ignored.** This is not general AttoLang GPU parser
support: the dedicated, opt-in compiler slice has its own restricted lexer.

The emitted OpenCL kernel is fixed, with explicit `__global const float`
inputs, writable output, `float alpha`, `ulong n`, 64-bit addressing,
and `#pragma OPENCL FP_CONTRACT OFF`. The compiler never interpolates
guest identifiers or arbitrary expressions into GPU source. The host-only
reference geometry checker rejects zero dimensions, axes beyond
trusted limits, workgroup quotas, shared-memory quotas, total thread quotas,
and 64-bit multiplication overflow.

## Three-buffer provider (stacked experimental implementation)

The existing `OpenClProvider` continues to support one-input, one-output
DOP `Scale/Bias/ScaleBias` operations. The stacked G4 implementation adds a
separate `SaxpyProvider` plus explicit `SystemApi::prepare_saxpy` and
`enqueue_saxpy_kernel` for X/Y/output, alpha and n. It must **never** route
SAXPY through the older two-buffer `Provider::begin` API. See
`ATTO_GPU_SAXPY_PROVIDER_G4.md` for its physical state machine and proof
gates. This implementation has not yet passed independent exact-head
C++/CTest execution or physical GPU verification.

The G4 provider adds physically distinct X/Y/output allocations, retained
staging memory, bounded one-queue resident quotas, and real OpenCL event
polling. It does **not yet** provide a shared tenant-wide quota ledger,
a general three-buffer atomic fence-claim API, a complete GPU launch
profile, actor-continuation wakeup, complete ownership/borrow integration,
or a verified physical device receipt. These remain release blockers.
Never free on a cancellation request, alias a writable output with an input,
or silently fall back to CPU execution.

## Evidence/promotion

- C++ unit checks: limited frontend, source-injection negatives, exact source
  shape, checked geometry. These are **host reference tests only**.
- Native compiler CLI CTest: positive `.atto` emission, host-effect negative
  with no output artifact, and normal-scalar-frontend no-fallback negative.
  This proves **source-to-OpenCL C artifact emission** only when executed.
- Physical GPU gate: exact head/tree SHA, compiler digest, precise fixture
  hash, device ID/capabilities, real queue allocations and 3-buffer ownership,
  native enqueue/fence receipt, independent f32 oracle and negative mutation
  runs. Must report nonzero executed steps and no skipped device tests.
  `steps=[]` or `steps=null` is `NOT_EXECUTED`, not success.

Related: `docs/GPU_PROVIDER_DISPATCH_CONTRACT.md`,
`docs/DOP_GPU_EXECUTION_BOUNDARY.md`, and shared
`oreslang-platform-conformance` draft #504. This slice deliberately does
not merge or modify the older provider and planner PRs.
