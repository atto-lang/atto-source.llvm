# Accelerator source admission v0 — CPU, GPU, quantum and actor pipeline boundary

Status: **compile-only source-to-typed-IR subset, not general Oreslang guest execution**.

This increment adds explicit, narrow source compilers in both backends:
- Java: `dev.oreslang.runtime.accel.AcceleratorSourceLowering`
- Native: `oreslang::accel_source` (`frontend/AcceleratorSourceLowering.hpp`)

## GPU v0 grammar and lowering

```ores
gpu fnc transform(f32 value): f32 {
    return value * 2.0 + 1.0;
}
```

The independent compile-only frontends accept one function declaration
with one f32 scalar parameter, returning f32 and a single expression in the
limited form `value [* finite_f32] [(+|-) finite_f32]`.
They reject other source constructs before lowering: no host I/O, actor
spawning, await, loops, arbitrary calls, FFI or implicit CPU fallback.
A caller **must** supply an existing checked `DataOrientedLayout.Plan`
(Java) or `dop::Plan` (native), which defines the rows, fields, scalar
size/alignment and AoS/SoA/AoSoA layout.

The result is `DopKernelIR.Kernel` / `dop::Kernel`: a typed artifact that
can execute on an **explicit CPU reference path** for conformance tests, or
emit OpenCL C source by calling `emitOpenCl` / `emit_opencl` with a
validated device profile. Emitting C source is **not** allocating device
memory, launching a GPU kernel, or validating real hardware capability.

A GPU worker must eventually use an authenticated device capability, a
real OpenCL provider, byte-/request-budget admission, generation-fenced
owned allocations, async device-event futures, cancellation and quiescence
barriers. The reference CPU executor cannot be silently invoked when the
user requests GPU execution.

## Quantum v0 grammar and lowering

```ores
quantum fnc bell(): Circuit<2,2> {
    h(0);
    cx(0,1);
    measure(0,0);
    measure(1,1);
}
```

The separate quantum source frontend accepts one circuit declaration with
literal qubit/classical widths, bounded X/H/CX and terminal measurement
statements. It produces `QuantumCircuitIR.Circuit` /
`quantum::Circuit`. The typed circuit can emit OpenQASM 3 or be passed to
the *explicit* ideal simulator. The simulator's small gate set and
qubit/shot quotas are not equivalent to a real QPU provider, device
connectivity validation, dynamic mid-circuit feedback or noise modeling.

## Integration and security invariants

1. **The main `Parser.parse` and the native LLVM source parser continue to
   reject `gpu`/`quantum` declarations.** Only these independent, explicit
   compile-only APIs admit the narrow subset. Do not claim language-wide
   syntax support or executable Oreslang accelerator functions yet.
2. No host access, actors, mailbox operations, borrowing, device pointers or
   general effects can enter the new IR via v0 source syntax.
3. GPU values are owned and shape-checked; device buffers cannot be shared
   via raw pointers in `ActorMail`. Cross-actor transfer requires a
   generation-checked owned lease and a bounded asynchronous credit.
4. Quantum simulator execution is an explicit test path and **never** a
   hardware fallback. Runtime hardware submission requires selected
   provider credentials and explicit budgets.
5. Keep compiled artifact identity/version associated with source span,
   backend profile, device descriptor and actor incarnation before
   enabling hot-reload or request-scoped caching.
6. Future `StreamOperator<CpuBatch,GpuResult>` must await device completion
   without blocking a carrier thread; it publishes only a fully owned
   result, and graph cancellation must quiesce the device before release.

## Current executable evidence

The separate source subsets and their existing typed IR dependencies were
mirrored into a funded GitHub Actions repository using Git blob identities
(no private cross-org checkout token). The full targeted test job compiled
native C++17 with Release assertions disabled, compiled Java 21, rejected
unsupported host syntax in both, executed the CPU kernels and Bell-state
simulator, compared CPU/QASM outputs, and compiled generated OpenCL C syntax.

Executed funded run:
https://github.com/zed-pkg-test/java-app/actions/runs/38099887344

This is **not** complete JVM/LLVM compiler, native AOT/hybrid, real GPU/QPU,
or production actor-pipeline certification. The source PRs remain drafts.

## Next implementation gates

1. Preserve source span/effect metadata and add the new accelerator nodes
   to the *real* Oreslang AST + type checker, not an unrelated parser route.
2. Compile per-target typed kernel IR with no generic host or actor effects,
   then verify LLVM device codegen, CPU SIMD, and real GPU device output.
3. Introduce provider capability/driver ABI with buffers, event fences,
   quotas and failure telemetry; verify device reset and cancellation.
4. Build explicit quantum provider adapters, connectivity/noise/shot
   metadata, provider cost budgeting, and QASM conformance.
5. Finish structured ActorGroup/ActorStream graph startup, credit-based
   transport, get_ready()/get_done() barriers, and release-once device
   buffer handoffs.
6. Run full current-head Maven/CTest, GraalVM AOT/hybrid/isolate and
   multi-tenant pipeline tests before any production merge.
