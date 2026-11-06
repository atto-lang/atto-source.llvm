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

## Mandatory device-provider integration (not implemented)

The existing `OpenClProvider` and `OpenClSystemApi` support the preexisting
one-input, one-output DOP `Scale/Bias/ScaleBias` operations. They **do not**
support this three-buffer SAXPY ABI. Do **not** route this new source artifact
through the old two-buffer `Provider::begin` API or claim that the old
hardware probe executes this kernel.

Before real SAXPY execution can be enabled, add provider-authenticated
three-buffer allocation and one atomic input-X/input-Y/output claim,
tenant/device/generation-fenced leases, checked byte bounds against
`n * sizeof(f32)`, 3D launch-profile/device limit admission, native
OpenCL program/arg binding, upload/kernel/download event ownership,
async actor-continuation wakeup, cancellation drain, and physical
release/quiescence. Never free on a mere cancellation request.
Output owners must not alias either input unless a future proof permits it.
Any device/provider absence is a hard failure, not a CPU substitute.

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
