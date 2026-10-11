# OpenCL + NativeActorGroup integration release gates (October 10, 2026)

## Current status: reference graph passed, guest actor pipe not released

The source compiler's trusted compile-only GPU F32 IR, OpenCL C lowering,
bounded GPU coordinator, generation/byte fences and CONTROL adapter are
implemented. The normal LLVM guest parser continues to reject unsupported
actor/GPU declarations; this prevents accidental fallback or unrestricted
host device access.

The authoritative native group is **NativeActorGroup** in
`src/main/cpp/oreslang/runtime/ActorGroupLifecycle.hpp` on current main.
The per-actor DATA/CONTROL scheduler and typed port admission are in draft PR **#127**:
https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/127
PR #97 was closed. Current main contains the prior CONTROL/port substrate, but group-wide CONTROL serialization is no longer the intended runtime model.

The earlier `ActorGroup.hpp` implementation in PR #54 was closed without
merging and is NOT a supported integration target. Accelerator PR #83's
integration tests now use NativeActorGroup. The obsolete PR #54-specific
test was deleted.

## What actually executed

1. [Current-main NativeActorGroup lifecycle and exclusive CONTROL tests](https://github.com/zed-pkg-test/java-app/actions/runs/38103402849).
2. [Real GPU completion callbacks delivered only via current NativeActorGroup CONTROL](https://github.com/zed-pkg-test/java-app/actions/runs/38103445931).
3. [Real OpenCL CPU-device conformance](https://github.com/zed-pkg-test/java-app/actions/runs/38102899806), explicitly selecting a CPU OpenCL driver and verifying AoS, SoA, AoSoA uploads, kernel compilation, NDRange dispatch, readback and f32 results. An explicit GPU-only probe returned SKIP because the runner has no OpenCL GPU.
4. [Bounded three-stage host pipeline against current-main NativeActorGroup](https://github.com/zed-pkg-test/java-app/actions/runs/38103535925). A CPU producer creates an owned batch, a GPU-stage actor submits a checked OpenCL kernel on an explicit OpenCL CPU device, its provider completion only posts reserved CONTROL, and a collector verifies output. Byte/frame credits are held until downstream/device acknowledgement; actor get_ready/get_done futures settle only at valid barriers.

These runs compile exact mirrored source Git blobs on a funded organization,
not cross-org private checkout with an insufficient GITHUB_TOKEN.
The new three-stage execution is a source-validated **restricted host graph**. The native compiler accepts a real Oreslang `.ores` fixture with declared StreamOperator ports, a typed GPU kernel and a source-level spawn/pipe expression, then dispatches predefined audited native actor implementations. It does **not** compile arbitrary actor method bodies or general guest-level actor effects.

### Additional executed integration gates

- [Java source-compiler parity](https://github.com/zed-pkg-test/java-app/actions/runs/38109615219): PASS on exact mirrored parser/compiler source and same .ores fixture; unrelated type-checker dependencies were test-stubbed.
- [LLVM source-derived OpenCL graph](https://github.com/zed-pkg-test/java-app/actions/runs/38105846277): PASS using the actual Parser.cpp + OwnershipChecker.cpp translation units and real explicit CPU OpenCL ICD.
- [Per-actor DATA/CONTROL scheduler with sanitizer stress](https://github.com/zed-pkg-test/java-app/actions/runs/38109985928): PASS, including typed mailbox-port admission, two-carrier actor separation and per-actor CONTROL priority.
- [Source graph with distinct DATA payload and CONTROL completion](https://github.com/zed-pkg-test/java-app/actions/runs/38109891583): PASS on explicit CPU OpenCL. The latest typed port helper rerun remains a separate gate.

## Device and actor invariants

- No automatic selection of CPU when GPU requested. The GPU-only probe
  skips without hardware, and the CPU OpenCL path is explicitly selected.
- The real OpenCL execution path allocates distinct input/output device
  buffers, compiles the emitted kernel, uploads bytes, dispatches and reads
  output, and acknowledges quiescent release. The host reference wraps it in
  an async provider worker; it does not yet expose all device events as a
  production driver service.
- Each actor has a reserved bounded CONTROL slot. Device completion carriers enqueue; admitted CONTROL and DATA turns share one exclusive logical lane per actor. Different actors can run concurrently even within one ActorGroup. Finalization cannot overtake active or
  queued CONTROL. A dropped CONTROL on close is flagged to the supervisor.
- The bounded native actor port moves ownership on admitted frames,
  retains ownership when full, and reserves frame and byte credit until
  explicit downstream acknowledgement after any device/loan fence.
- `get_ready()` is an initialization or provider-acceptance barrier;
  `get_done()` is termination or provider-device-release barrier.
  Neither should block a runtime actor carrier.
- A failed device release or uncertain kernel admission keeps credits and
  forbids resource reuse. Pending CONTROL delivery failures must be reported
  and escalated by a production supervisor.

## Required release tests

- [ ] Integrate and review the native per-actor DATA/CONTROL scheduler from draft PR #127. Preserve the one authoritative NativeActorGroup registry.
- [ ] Lower actual Oreslang GPU/quantum AST and typed actor stream operators
  through full native type and borrow checking, producing executable
  guest-level `CpuWorker pipe GpuWorker pipe CollectResultsWorker`.
- [ ] Execute a physical **GPU** OpenCL device with exact f32 output on
  AoS/SoA/AoSoA, explicit device identity, quota, reset and cancellation;
  do not count the verified CPU OpenCL ICD as a GPU result.
- [ ] Run slow-sink, queue saturation, actor supervisor panic/restart,
  multi-tenant/cross-device transfers, cancellation during dispatch and
  shutdown, device loss, stale generation and duplicate event stress.
- [ ] Current-head complete native CMake/CTest, full Java Maven
  TypeChecker/OwnershipChecker, Graal AOT/hybrid/isolate and native
  LLVM release/sanitizer builds; targeted exact-blob green does not substitute.
- [ ] Implement an explicitly selected QPU backend, hardware/circuit budget,
  native gate map and measurement verification; the ideal simulator is not QPU
  validation.

Keep accelerator PR #83 and native scheduler PR #127 **draft** until the remaining compiler, hardware and release test gates are satisfied.
