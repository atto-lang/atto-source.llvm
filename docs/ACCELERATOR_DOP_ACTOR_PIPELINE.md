# Data-oriented layout, GPU and quantum effects, and typed actor pipes

Status (2026-10-10): **design and checked layout substrate only**. This is not
a claim of executable GPU kernels, quantum hardware integration, or production
actor-pipe execution. Both compilers reserve `gpu` and `quantum` and reject
unimplemented execution **without implicit CPU fallback**. LLVM additionally
rejects actor/async source because it lacks native actor frames.

## Separate four concerns

1. **Actors:** scheduling, lifecycle, cancellation, supervision, ownership and
   bounded `ActorMail` transport. They remain host-coordinated even when
   their payload is computed by a GPU or QPU.
2. **Data-oriented representation:** homogeneous primitive columns and tiles,
   explicit layouts, vectorization, shape proofs, deterministic ownership.
3. **GPU compute:** explicit GPU effects, device-capability admission,
   executable kernel IR, transfer/launch/synchronization futures and resource
   limits. GPU computation must never silently run on a CPU.
4. **Quantum compute:** circuit-oriented typed IR, gates/qubits, shots,
   measurement and explicitly selected simulator or hardware provider.
   Quantum workloads are *not* ordinary parallel GPU kernels.

## Checked layout ABI v0 (implemented in both backends)

Java: `dev.oreslang.runtime.DataOrientedLayout.Plan`.
Native: `oreslang::dop::Plan` in `DopLayout.hpp`.

This is a **host-side geometry calculation**, not an allocator, Oreslang
user-level API, or a device memory ABI. Each buffer has homogeneous fixed-width
POD fields and six descriptor inputs: `kind, rows, fields, scalarBytes,
alignment, lanes`. Each layout has these exact byte offsets:

- AoS: `row * align(fields*scalarBytes, alignment) + field*scalarBytes`.
- SoA: `field * align(rows*scalarBytes, alignment) + row*scalarBytes`.
- AoSoA: `(row/lanes)*align(fields*lanes*scalarBytes, alignment) +
  field*lanes*scalarBytes + (row%lanes)*scalarBytes`.

The total byte length includes final field/tile padding, not merely the
address of the last scalar. `lanes` must be 1 for AoS and SoA, otherwise a
power of two up to 1024. Scalar width/alignment must be compatible powers of
two, scalar width <= alignment, and alignment <= 4096. Zero-row buffers have
zero byte length. Oversized layouts and invalid indexes fail before allocation.
Native layout sizes are capped at signed-64-bit range for Java parity.
These restrictions define v0, **not** GPU device-alignment guarantees.

Sample parity fixture: AoS(3,2,4,16,1) = 48 bytes and (2,1) = 36;
SoA(3,2,4,16,1) = 32 bytes and (2,1) = 24;
AoSoA(5,2,4,16,4) = 64 bytes and (4,1) = 48.

A future `Array<T,Layout>`/ `Table<T,Layout>` type must keep field widths
and record shape in a versioned typed ABI. Heterogeneous field widths, nested
values, packed booleans, GPU buffers and pointer-bearing objects MUST NOT be
reinterpreted with this v0 homogeneous-layout descriptor.

## Pipeline language target (ILLUSTRATIVE, NOT EXECUTABLE)

The staged StreamOperator syntax is being reviewed separately in Java
PR #544 (it is currently compiler-only and fail-closed). A fully typed
three-stage graph should be expressible as:

```ores
// Examples assume the future typed ActorStream and device-buffer APIs.
define actor CpuWorker implements StreamOperator<void, CpuBatch> as
  on_start(): void {
    // Produce an owned immutable/transferable SoA batch.
    self.send(build_cpu_batch());
  }
end

define actor GpuWorker implements StreamOperator<CpuBatch, GpuResult> as
  async receive(ActorMail<CpuBatch> mail): void {
    const deviceBatch = await Gpu.upload(rt take mail.value);
    const result = await Gpu.run(transform_kernel, rt take deviceBatch);
    self.send(await Gpu.download(rt take result));
  }
end

define actor CollectResultsWorker implements StreamOperator<GpuResult, Results> as
  receive(ActorMail<GpuResult> mail): void {
    self.send(summarize(mail.value));
  }
end

pub fnc do_work(): ActorStream<void, Results> {
  return spawn CpuWorker()
      pipe spawn GpuWorker()
      pipe spawn CollectResultsWorker();
}
```

If `CollectResultsWorker` actually consumes without emitting, it is a
`StreamOperator<GpuResult, void>`, and the complete graph type becomes
`ActorStream<void, void>`. A bare `ActorStream` erases the critical port
contract and should not be preferred.

The reserved lowercase `gpu`/`quantum` keywords cannot also name API values; this example uses the prospective `Gpu` runtime namespace.\n\nThe **actor** is a host-side logical process, *not* itself a GPU thread. A
GPU worker enqueues an explicit kernel, yields on `await`, and resumes an
admitted actor continuation when the device event completes. A source/consumer
actor may live on different host runtimes; actor boundaries remain protected.

## Proposed execution syntax (reserved, NOT enabled)

```ores
gpu fnc transform_kernel(DeviceView<float, SoA> input): DeviceBuffer<float> {
  // GPU-only, statically bounded, device-safe instructions.
}

quantum fnc bell_pair(Qubits<2> q): Circuit<2> {
  // Circuit-building gate operations; no ambient I/O or arbitrary heap.
}

async fnc schedule_work(CpuBatch data): Results {
  // gpu { ... } and quantum { ... } are explicit accelerator effects.
  // The future API must require a selected backend/capability.
}
```

Prefer compiling GPU functions to validated device IR with explicit target
profiles (e.g., SPIR-V/PTX via pluggable backends). LLVM host IR is **not**
itself GPU kernel code. For quantum, generate a typed circuit IR and use an
explicit QIR/OpenQASM-oriented provider path where appropriate; never pretend
a conventional LLVM function is a circuit. Neither example is a test fixture
for successful compilation until these backend gates exist.

## Required compiler/lowering gates

- **Effects:** disallow host I/O, thread spawning, actor/mailbox operations,
  hidden allocations, arbitrary reflection/FFI, and unbounded recursion in
  accelerator kernels; report unsupported operations before code emission.
- **DOP:** prove element shape, alignment, alias sets, contiguous/strided
  accesses, integer overflow, exclusive write regions, and valid vector widths.
  AoS/SoA/AoSoA conversion must be explicit when it changes ownership or
  allocates. Fusion/vectorization are legal only when effect and alias proofs
  permit them.
- **GPU:** require declared target features, device capability, launch
  bounds, register/local/shared memory budgets, copy/transfer accounting,
  deterministic completion signals, error propagation, and kernel verifier.
  Device pointers are never ordinary actor-sendable references.
- **Quantum:** typed gate set, qubit allocation/lifetime, no-cloning,
  measurement-to-classical boundary, provider-specific connectivity,
  circuit-depth/qubit/shot quotas, calibration/noise metadata and explicit
  simulator-versus-hardware selection. No unannounced classical fallback.
- **Actor pipes:** exact stream port types, single-consumer edge ownership,
  bounded byte- and message-credits, backpressure without carrier-thread
  blocking, EOF/error propagation, startup reservation, graph cancellation,
  device-event drain, release-once buffer leases and generation fencing.
  A zero-copy edge may exist only within a proven compatible device/isolate
  memory domain and must be lease-mediated, never a raw pointer in a message.
- **Security:** untrusted actors cannot borrow host/device pointers, bypass
  device quotas, execute arbitrary GPU binaries or submit unrestricted
  provider credentials. Explicit capabilities and per-tenant budgeting apply.

## Implementation sequence and release gates

1. Land checked DOP layout parity, keyword reservation and fail-closed
   tests in Java and native (this branch).
2. Introduce typed DOP collections, borrowed `View`/owned `Buffer`,
   deterministic region lifetime, shape/alias proofs and CPU AoS/SoA/AoSoA
   vectorized lowering. Compare exact results across both compilers.
3. Build GPU kernel/effect AST+IR, safe compiler admissions and at least
   one executed GPU backend with CPU reference *testing* (not fallback).
4. Implement bounded asynchronous actor stream transports from the typed
   pipe draft, then device-lease migration and full cancellation fencing.
5. Implement simulator-first quantum circuit IR and hardware provider
   interface with reproducible shot/measurement traces and quotas.
6. Certify with golden cross-compiler layout/round-trip properties,
   fast/slow-consumer pipeline tests, GPU OOM/device reset, cancellation
   during copies/kernels, simulated quantum backend errors, multi-tenant
   isolation and benchmark baselines. Never mark a path production-ready
   on source-only/typecheck-only or zero-step CI evidence.

Related Java draft: https://github.com/ores-truffle-oreslang/oreslang-source.java/pull/544
