# AttoLang GPU SAXPY security audit — 2026-11-06

Scope: LLVM draft PR #1487, STACKED on source-to-OpenCL artifact PR #1466.
This is an implementation/host-review audit, **not** an independent security
approval, native compiler pass, or physical-device conformance certificate.
Canonical language inputs use `.atto`; the restricted G4 SAXPY profile has
neither full GPU MIR nor general CUDA-class language coverage.

## Findings and dispositions

| Severity | Finding | Disposition | Release evidence needed |
| --- | --- | --- | --- |
| Critical | Destroying a provider with a pending nonblocking OpenCL upload/download dropped referenced host staging vectors without guaranteed device quiescence | **Mitigated, not production-resolved**: explicit drained `close()`, no reopens, active destructor invokes `std::terminate` before memory destruction; POSIX isolated negative/death test added | Supervisor-managed async shutdown, preserved staging during device loss, no process-wide crash for tenant faults |
| High | Hardware CTest could call a source-only reference probe without running `oreslang-llvmc` device emission | **Fixed in tests**: hardware CTest emits OpenCL from the actual native compiler and passes exact bytes to a required 3-argument probe | C++/CTest must execute on exact head; real GPU result must not be skipped |
| High | Receipt script accepted arbitrary caller-supplied compiler/probe executables, permitting forged pass text | **Hardened**: exact committed Git archive, clean HEAD, fixed tracked fixture blob, fresh CMake builds and native host tests, no supplied binaries | Independent trusted toolchain/runner attestation, nonzero executed steps, external physical hardware inventory |
| High | `SystemApi::try_create_gpu` swallowed all OpenCL ICD/device initialization errors as no-GPU skips | **Fixed in source**: typed `NoCompatibleGpu` for absent/unqualified GPU only; unexpected driver/platform errors propagate as failures | Error-injection tests and real multi-platform ICD execution |
| Medium | First enumerated GPU chosen even if incompatible when another compatible GPU was available | **Fixed in source**: scan candidates for address-width/f32/endianness/buffer profile; broken capability queries remain hard errors | Multi-GPU fake-ICD tests and independent hardware inventory |
| Medium | Probe printed `event_stages=4` as an unverifiable constant | **Fixed in source**: count four actually completed and released native OpenCL events; require count before Success; fake driver regressions check 1/3/4 sequences | Native hardware event logs and compiled test receipts |
| High | Per-provider queue quota is not an enforceable shared tenant/device resource quota | **OPEN — blocks production guest admission** | Shared trusted tenant ledger, atomic multi-provider accounting and exhaustion tests |
| High | Three-buffer physical allocations do not yet carry the language's actor-owned `GpuBufferFence`/borrow capabilities and atomic three-owner claims | **OPEN — blocks production guest admission** | Compiler-owned borrow/effect graph; authenticated X/Y/output fencing, cancellation and generation tests |
| High | Host-only borrowed `SaxpyApi&` lifetime and shutdown cannot guarantee that device/queue survives every operation outside the current probe's stack discipline | **OPEN — blocks production guest admission** | Shared supervisor-owned device lifetime, stable queue generation, quarantine registry, async cleanup |
| High | No nonparking actor `gpu task` continuation, Future ownership, tenant cancellation or supervisor restart/recovery bridge | **OPEN — blocks production guest admission** | Exact-source compiled actor GPU tests, cancellation model, bound scheduler receipts |
| High | A driver-reported `CL_DEVICE_TYPE_GPU` and device name are not cryptographic hardware attestation | **OPEN — blocks physical assurance certification** | Trusted runner device inventory and device/driver provenance bound to completion logs |
| High | Original-source native C++/CTest and physical GPU lanes lack nonzero executed-step evidence | **OPEN — NOT_EXECUTED** | Exact head SHA, tree, compiler digest, job attempt, positive executed steps, independent reviewer |
| Scope | Only closed typed SAXPY is compiled by a separate opt-in parser; no normal LLVM GPU MIR, generalized kernels, streams or CUDA synchronization | **OPEN by design** | Unified AttoLang HIR/MIR effects and independent cross-backend conformance |

## Normative safety checks

- Never call OpenCL `clReleaseMemObject` while the corresponding asynchronous
  host/device operation may still reference its buffer or host staging. For
  a nonblocking write, the OpenCL specification prohibits reusing host memory
  until the command finishes.
- The physical GPU provider never silently selects CPU execution. Absence of
  a compatible GPU is a SKIP for diagnostic CTest and a FAILURE for strict
  release assurance; unexpected ICD errors are failures in either path.
- A `clFinish` witness may be used **only** from a host control/recovery
  context because it blocks until all earlier queue commands complete. It
  must not run on an actor carrier.
- Null/unknown events, device loss and ambiguous enqueue failure quarantine
  exact allocations. Timeouts and cancellation do not imply physical
  completion; retained quota and resources remain unavailable for reuse.
- The program and all three buffers must be physically retired before
  reporting a successful output to a guest. The four measured device events
  are necessary but are not, by themselves, a cryptographic device receipt.
- A death-test process termination protects against host UAF in this
  experimental provider; it is **not acceptable as tenant isolation** and
  must be replaced by supervisor-owned nonblocking cleanup before shipping.

## Provenance and execution classification

The current GitHub workflow `Atto GPU SAXPY source and provider host checks`
compiles focused C++17 host reference tests with ASan/UBSan and verifies exact
PR head/tree. Its output class is `HOST_ONLY_SANITIZERS`, not a GPU run.
The dedicated CTest hardware test now binds source to compiler-emitted OpenCL
C bytes. The strict hardware script creates fresh binaries from an exact Git
archive and requires real device marker plus an independent f32 oracle.

At the time of this audit, hosted runs on LLVM GPU branches have reported
`steps=null`: this is runner admission failure, **not** a source/compiler
pass or fail. No claim of locally executed C++/CTest, real device dispatch,
full CUDA parity or independent approval is made in this document.

Reference: Khronos OpenCL unified reference pages for `clEnqueueWriteBuffer`,
`clReleaseEvent`, and `clFinish`.
