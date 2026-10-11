# Accelerator compiler and asynchronous actor/device integration

Status October 10, 2026: this is a **tested compiler and device-lifecycle
integration slice**, not runnable GPU/QPU hardware and not production actor
pipeline transport. Keep draft until the release gates listed below execute.

## Source to compiler AST to typed device IR

The normal Java compiler entry point `OresCompiler` now exposes trusted
*compile-only* boundaries `compileGpuSource(...)` and
`compileQuantumSource(...)`. The full Java `Parser` carries immutable,
explicit `Ast.GpuF32Decl` and `Ast.QuantumCircuitDecl` records, which
are **not** executable `Ast.Decl` variants and cannot enter an
`Ast.Program` through the normal guest parse route.

The existing strict accelerator source compiler validates the narrow F32
scalar transform and X/H/CX/terminal-measurement circuit grammar. The main
Parser creates typed AST declarations; `OresCompiler` reconstructs checked
DOP kernel / circuit IR from these AST nodes and emits OpenCL C or returns
a typed circuit. The normal `OresCompiler.parseAndTypeCheck()`,
`Parser.parse()`, and isolate guest admission still reject source-level
`gpu` and `quantum` declarations.

The LLVM `Parser.hpp/.cpp` now exposes
`compile_gpu_source(...)` / `compile_quantum_source_openqasm3(...)`,
using the same native bounded compiler and native typed IR. Its normal
`parse_program` remains a checked i64-only guest subset and rejects
accelerators. This means codegen APIs exist but the LLVM backend does not
compile ordinary Oreslang GPU functions into LLVM IR or actually launch
an OpenCL driver.

Both compilers are tested against exact mirrored source Git blobs and
explicitly invalid guest source.

## Provider-neutral GPU dispatch, byte/frame credit, and fences

Java `GpuDispatchCoordinator` and LLVM `gpu::GpuDispatchCoordinator`
accept previously typed, device-profile-admitted `OpenClArtifact` values.
A selected *trusted host* provider implements device launch and physical
resource release. Neither coordinator discovers or authenticates a real
OpenCL provider; source code may not instantiate an actual hardware device
by providing arbitrary tenant/device strings. Provider credentials and
device handles belong to the host and must be isolated from guest code.

Each request:
1. Reserves two complete device-sized buffers (read-only input and owned
   distinct output) against positive byte and concurrent-frame limits.
   Submission beyond credit is rejected, not placed in an unbounded queue.
2. Begins a generation/sequence-checked opaque device fence.
3. Exposes `get_ready()` when the host provider accepts submission, and
   `get_done()` only after the real device completion event **and**
   the provider-confirmed physical release barrier.
4. On cancellation, poisons the in-flight fence and waits for actual
   completion before calling physical release. Cancellation never refunds
   credit early or permits a device buffer to be transferred.
5. On failed launch acceptance or uncertain physical release, retains the
   reservation, reports failure and denies close/reuse. A separate trusted
   recovery operation is required to reconcile a stuck device allocation.
6. On verified release, retires the fence and refunds quota exactly once.
   The C++ implementation explicitly rejects duplicate provider callbacks.
   Failed device kernel execution also waits for resource retirement.

The GPU provider callback may run on ANY native carrier thread. Java
`GpuActorCompletionBridge` turns the completion into a **reserved CONTROL
enqueue** for the receiving actor's logically single-flight scheduler
lane. Only the scheduler-admitted control turn completes the value future;
an enqueue failure is recorded but does not run user callbacks on the
device thread. This is a bridge contract, not a fully integrated
`ActorGroup` scheduler or actor `on_panic` dispatcher. The native
coordinator currently exposes a completion future; connecting it to a
reserved native actor CONTROL slot is a separate gate.

## Executed evidence

Exact-source funded validation uses `zed-pkg-test/java-app`, mirroring
source Git blobs without tokenized private cross-org checkout. Tests compile
LLVM's real `Parser.cpp` and native `OwnershipChecker.cpp`, then run
normal guest rejection and trusted accelerator code-generation integration.
The Java targeted job compiles real `Ast.java`, `Parser.java`,
`OresCompiler.java` and the accelerator runtime classes with Java 21.
The Java isolated harness contains **test-only stubs** for unrelated full
TypeChecker, OwnershipChecker, isolate policy and tree-shaker components;
it cannot certify those classes or the full Maven/Graal suite.

Java tests also simulate delayed provider device events, quota saturation,
cancellation, device failures and failed release. Native C++ tests execute
the equivalent provider completion and duplicate-release protections under
`-O2 -DNDEBUG -Werror`. A Java actor-control regression proves that an
actor callback is not invoked directly by a device event.

## Not yet implemented or certified

- Full Java TypeChecker and OwnershipChecker accelerator effects and source
  declarations within a normal multi-declaration Oreslang program.
- Real OpenCL driver/provider library, kernel compilation on hardware,
  actual upload/download/device allocation, hardware-level cancellation,
  numerical device results and latency/cost measurement.
- Real QPU provider/backend, connectivity and native gate validation,
  provider cost/shots/retry controls, noise-aware simulation.
- A verified bounded `ActorStream` graph scheduler connecting three
  production actors with typed port claims, mailbox byte/frame credits,
  `get_ready()`/`get_done()`, quiescent device transfer and supervision.
- Native actor CONTROL-slot bridge (do not run actor user code on the GPU
  callback carrier).
- Full source/current-head Maven, LLVM CMake/CTest and Graal native-image
  AOT/hybrid/isolate/release gates, plus real device tests.

The next integration step should exercise the real source compiler/type and
ownership checker against combined actor and accelerator declarations, wire
native actor CONTROL scheduling, then connect a device provider with real
device events. Preserve no-fallback behavior until hardware admission and
every release test are green.
