# Oreslang typed accelerator IR v0: CPU, OpenCL, quantum, actors

Status (October 10, 2026): **an implemented backend-neutral library IR, not
general Oreslang source-level accelerator execution**. Two language runtimes
now provide bounded DOP kernels, concrete CPU execution, real OpenCL C device
source emission, typed circuit/QASM emission and an ideal statevector
simulator. `gpu fnc` and `quantum fnc` **still fail closed in the frontends**.
This distinction is part of the safety contract.

## Implemented paths

| Feature | Java/Truffle | Native C++/LLVM companion |
| --- | --- | --- |
| Homogeneous POD layouts | `DataOrientedLayout.Plan` | `dop::Plan` |
| Typed F32 kernel | `accel.DopKernelIR.Kernel` | `dop::Kernel` |
| Executable host lowering | `DopKernelIR.executeCpu` | `dop::execute_cpu` |
| OpenCL C lowering | `DopKernelIR.lowerOpenCl` | `dop::lower_opencl` |
| Exclusive host-buffer transfers | `HostBuffer.take/borrowRead` | `HostBuffer::take/borrow_read` |
| QASM3 lowering | `QuantumCircuitIR.lowerOpenQasm3` | `quantum::lower_openqasm3` |
| Ideal simulator | `simulateProbabilities/Shots` | `simulate_probabilities/shots` |
| Opaque device fence ledger | `GpuBufferFence` | `gpu::BufferFence` |

These APIs are compiler/runtime implementation building blocks. They are not
yet imported as public Oreslang standard-library symbols and do not grant host
or device permissions to an untrusted guest.

## DOP typed kernels

The first admitted source-neutral kernel IR is a fixed field-wise F32
transform, with operations SCALE, BIAS, SCALE_BIAS. Input and output have
*identical* exact `kind/rows/fields/scalarBytes/alignment/lanes` shapes.
The output is a distinct owned buffer. Each logical (row, field) maps to one
proved in-bounds scalar byte offset under the v0 AoS, SoA or AoSoA rules.

CPU execution uses those same checked offsets rather than assuming contiguous
packed arrays. Java uses a single-owner direct ByteBuffer and scoped read
loans. Native uses byte storage and `memcpy` (avoids strict aliasing and
unaligned type-punning). Both reject write-while-borrowed and moving while a
read loan exists. The native HostBuffer also locks the wrapper-owned
handle separately from its State, so concurrent borrow/take/close and move
assignment cannot race on the state_ shared_ptr. Public C++ move constructors
and move assignments enforce borrow/closed-state exclusion directly, not only
through `take()`. Moves cannot overwrite live buffers, abandon outstanding
loans, or transfer a still-borrowed source. The exact-source 128-round
borrow-versus-transfer regression passes GCC/Clang ASan+UBSan and GCC TSAN.
Physical Java direct-buffer release remains GC-mediated;
logical close is not yet explicit native memory deallocation.

GPU lowering emits an **actual OpenCL C 1.2 kernel** with two-dimensional
global dimensions (rows, fields), checked precomputed strides, F32 values and
`FP_CONTRACT OFF`. Native float constants use round-trippable scientific
notation rather than fixed precision; Java constants use round-trippable F32
notation. Clang syntax-checks the generated OpenCL C from both backends.

The required `GpuTarget` profile currently only accepts 64-bit address
computations, enabled F32, and explicit row/byte limits. Admission is necessary
but not sufficient for actual hardware: there is no OpenCL driver integration,
hardware buffer allocation, command queue, device occupancy planner, local
memory allocation, transfer/memory pinning, SVM or dynamic binary loading.
The current kernels are scalar per work-item; CPU execution is not claimed to
be auto-vectorized or GPU execution optimized. Actual device performance and
correctness are not yet measured.

## Opaque device ownership and cancellation

`GpuBufferFence` / `gpu::BufferFence` model a **host-side metadata ledger**,
not the device allocation itself. A real provider must construct/use the
ledger alongside its opaque allocation and authenticate runtime capabilities.
Strings/identity labels in this prototype are comparison guards, *not*
cryptographically unforgeable authorization. In native C++, a Ticket now
holds a private, unique per-submission capability retained by the owning
BufferFence while in flight; matching numeric generation and sequence alone
can no longer complete or cancel another buffer's work. Stale completion
and cancelled-event acknowledgments invalidate that capability. The Java
backend uses opaque per-submission Ticket object identity and keeps provider
transitions package-private.

These tickets are **bearer capabilities, not proof of physical GPU completion**.
Only a trusted backend should call `complete`, `acknowledge_cancelled_device_event`
or `retire_after_backend_release` after independently authenticating the
device event and release. The C++ API still exposes these functions to native
callers, so trusted-module/FFI confinement remains an unimplemented release
gate; ticket possession alone is not sufficient security.

Lifecycle:

```
IDLE --begin(ticket)--> IN_FLIGHT --complete(success)--> IDLE
                                  --complete(failure)--> POISONED
             --cancel(ticket)--> POISONED (device event outstanding)
POISONED --backend acknowledged cancelled event--> POISONED (quiescent)
IDLE/POISONED (quiescent) --provider-released--> CLOSED
```

Taking a buffer owner is permitted only while IDLE, and invalidates the
predecessor handle. Native BufferFence now locks the wrapper's owner handle
before entering shared State operations, with fixed lock order
`owner_lock_ -> State::lock`. This serializes owner transfer against command
admission/completion and closes a shared_ptr move/use race. Native move assignment refuses to overwrite an unretired handle;
direct `std::move` construction and assignment are also subject to the same
IDLE-only constraint as `take()`, so in-flight tickets cannot be transferred
through implicit C++ moves. The regression tests reject both bypasses while
preserving the original live owner. GCC/Clang sanitizer and ThreadSanitizer
regressions exercise competing submit/take operations. Stale generations, foreign device/tenant identities and
duplicated command tickets are rejected. A cancelled operation cannot
release/reuse storage until the backend confirms device event completion.
There is no implicit recovery from POISONED, no actor-message serialization of
raw GPU pointers, and no guest-controlled device handle export.

No actor can yet directly send such a device buffer to another actor: a future
`DeviceTransfer<OwnedBuffer>` must be a separately admitted, same-device
capability, with byte-accounted credit and exactly-once owner transfer. For
cross-device, cross-isolate or remote actor edges, use an owned serialized
CPU buffer and explicit upload; prevent accidental zero-copy claims.

## Quantum circuits and effects

First quantum IR supports X, H, CX and terminal measurement, with 1..16
qubits, 1..16 classical bits, and <=1024 gates. Invalid control-target
overlap, repeated measurement, duplicate classical result indices,
out-of-range registers, post-measurement gates and unmeasured circuits are
rejected before emitting QASM or running the simulator. Native C++ also rejects
unknown enum discriminants supplied by casts/FFI, matching Java's closed enum
contract instead of silently discarding invalid operations. The simulator
executes an ideal real-valued statevector, returns exact marginal
probabilities, and offers deterministic seeded shot sampling (up to 1M
shots). Circuit builder admission rejects the 1,025th gate before
retaining an unbounded intermediate list. Seeded sampling builds a validated
CDF and uses upper-bound binary search, with complexity
O(2^classicalBits + shots * classicalBits) rather than
O(shots * 2^classicalBits). Zero-probability outcomes cannot be selected,
and invalid probability mass fails closed. The native quantum regressions
run under ASan/UBSan on both GCC and Clang in the standalone ABI/DOP gate.
No noise model, complex phase gates, mid-circuit measurement,
connectivity map or error correction is implemented. The QASM output uses
OpenQASM 3.0 and the standard gate library.

A future hardware provider must be **explicitly selected** and constrained
by tenant quotas, device connectivity, native gate set, duration/cost/shot
limits, provider credentials and measured device provenance. A simulator
is an explicit testing backend, never an implicit hardware fallback.
Quantum circuits are not GPU threads or general-purpose actor kernels.

## Actor pipelines — target, not yet executable end-to-end

```ores
pub fnc do_work(): ActorStream<void, Results> {
    return spawn CpuWorker()
        pipe spawn GpuWorker()
        pipe spawn CollectResultsWorker();
}
```

`CpuWorker` outputs an owned `CpuBatch`; `GpuWorker` accepts exactly that
port type, yields while the device queue runs, and only publishes an owned
`GpuResult` after its fence completes; `CollectResultsWorker` consumes
that type and publishes `Results`.

The graph needs production `StreamOperator<I,O>` type admission, bounded
async mailbox credits for *bytes and frames*, ready/done/cancel/drain futures,
single-consumer port claims, no borrowed references across actor turns,
quiescent GPU device release after cancellation, and no native carrier-thread
blocking during device waits. The previous Java typed pipe draft remains
compiler-only and fail-closed. Do not enable the expression by forwarding
outputs with fire-and-forget `ActorRef.send()`.

## Current test certification

Funded GitHub Actions mirrors **exact changed Git blobs** from the two
private source branches; it does not checkout a cross-org private repository
using an Actions GITHUB_TOKEN. Targeted jobs compile native C++17 with
`-O2 -DNDEBUG -Werror` and Java 21, run layout/CPU IR/ownership/quantum
tests, compare CPU results and generated QASM across implementations, and
syntax-check generated OpenCL C with Clang. It is not an exact *full-tree*
mirror and does not certify full Maven, LLVM IR generation, Graal native AOT,
hybrid/isolate, GPU execution, or hardware QPU results.

## Remaining release order

1. Compiler source `gpu`/`quantum` AST nodes and effect checking; lower
   only validated source into the typed kernel/circuit IR, with no fallback.
2. Actual OpenCL runtime provider: allocate opaque buffers, copy/dispatch,
   capabilities, timeouts, device-event fences, cleanup and GPU-backed tests.
3. Optimize CPU IR with proved alias analyses and SIMD, add typed F64/integers,
   elementwise expressions, reductions, tiling and kernel fusion.
4. Provider-selected QPU and simulator adapters with independent OpenQASM
   conformance and backend-cost admission.
5. Bounded typed actor pipe graph execution with physical release/barrier
   verification and slow-sink, cancellation and multi-tenant stress tests.
6. Execute full current-head source CI and native AOT/hybrid/integration suites
   on the *exact revisions* before merging non-draft accelerator support.
