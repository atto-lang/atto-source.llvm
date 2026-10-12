# DOP-aware GPU admission and CPU/GPU nested scopes

This document supplements the execution-domain proposal in
`oreslang-source.java` PR #700 and `oreslang-source.llvm` PR #229.

**Implementation status:** These are trusted Java/LLVM runtime **preflight
building blocks only**, not source-level `cpu {}`/`gpu {}` execution.
Java DOP is based on the stacked branch for PR #579; on LLVM the typed DOP
runtime exists on main. A normal Oreslang user program cannot bypass the
compiler's fail-closed execution-domain gate by invoking these APIs.

## DOP is the representation that crosses host/device boundaries

Each GPU kernel and each device buffer is associated with one exact
`Plan`: `(AoS|SoA|AoSoA, rows, fields, scalarBytes, alignment, lanes)`.
Using only an allocation's size is unsound: differently laid-out plans
can have equal padded byte lengths but different field offsets.

A trusted backend must create a typed buffer descriptor when it allocates
actual device storage. `DopGpuAdmission` / `dop::preflight_gpu`
check, without transferring memory:

- identical full layout metadata between kernel, input and output;
- homogeneous F32 kernel and valid device target, explicitly 64-bit today;
- nonzero rows and byte length; GPU zero-work launches are rejected;
- exact padded bytes of **each** device allocation;
- two distinct device owners and two distinct, nonzero generations;
- matching tenant and device IDs; neither fence may be in flight or poisoned;
- `2 * bytesPerBuffer` within the signed 64-bit reservation ABI.

The returned descriptor is only a *preflight snapshot* (not a capability).
It cannot prove that bytes were uploaded, a device was allocated, a fence was
acquired, the data is initialized, or the caller owns provider credentials.
An authorized provider must repeat checks atomically while claiming the
two allocations and starting physical device operations. During any in-flight
period, no external mutable host/device aliases may survive. Cancellation
does not free either buffer until an authenticated device event confirms
quiescence. This preflight neither schedules GPU work nor chooses a CPU
fallback.

## Lowering a nested CPU/GPU expression

Future compiler lowering must consume the same typed DOP kernel/layout IR:

1. A CPU region produces an **owned** `HostBuffer<Layout, F32>`.
2. Explicit `Gpu.upload` consumes an allowed owned host buffer or a
   read-only, lifetime-pinned transfer lease; it creates an authenticated
   `DeviceBuffer<Layout, F32>` and returns an async completion.
3. The DOP/GPU admission described above verifies the kernel and both device
   buffers before provider submission, under the provider's authoritative
   ownership/capability checks.
4. `gpu { cpu { ... } }` splits the device work into a completion dependency
   followed by a CPU continuation. Required results need an **explicit**
   `Gpu.download` and a release/transfer fence; the host continuation does
   not run in an OpenCL kernel, nor on an arbitrary actor's carrier thread.
5. A nested return to GPU requires an explicit new upload or owned device
   capability and another checked launch. Compiler fusion of adjacent DOP
   kernels is legal **only** after preserving exact layout, alias/effect and
   sequencing proofs.

The CPU `DopKernelIR.executeCpu` remains an explicitly selected CPU
backend. An unavailable GPU cannot silently call `executeCpu`, and a
zero-row host kernel does not authorize an empty device launch.

## Release gates

- Wire trusted `DeviceBuffer` creation to real provider allocation provenance
  and atomically claim both fences, not merely snapshot them.
- Enforce owned host buffer transfer, initialized output, asynchronous
  upload/download barriers, actor scheduling and cancellation quiescence.
- Extend device capabilities beyond the current explicit 64-bit/F32 profile
  only after validating per-device address/index, alignment and layout ABI.
- Run exact-source Java and native C++ targeted tests plus shared vectors,
  sanitizer/concurrency tests and real GPU execution; a metadata-only
  preflight is not device execution verification.
