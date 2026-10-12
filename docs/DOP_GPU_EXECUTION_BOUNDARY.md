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

## Paired fence claim (implemented in this branch)

After typed kernel/layout preflight, `DopGpuAdmission.claim` (Java) or
`dop::claim_gpu` (LLVM) invokes an atomic **two-buffer fence claim**.
The allocation locks are acquired in stable order (Java monotonic allocation
ranks; native ordered owner and state pointers). Single-buffer `take`,
`begin_command`, completion, cancellation and retirement synchronize
with the same allocation locks. Before touching either status the claim
checks both exact padded byte lengths, distinct device allocations and
generations, tenant/device equality, `IDLE` and backend quiescence, and
nonwrapping ticket sequences. Only after both return tickets have been
created do the statuses change to `IN_FLIGHT` together.

Failed validation does not partly claim either buffer. A successful claim
blocks reuse/ownership transfer until corresponding authenticated device
events are acknowledged. Reversed-order concurrent pair claims are covered
by regression tests. Java additionally preconstructs single-buffer tickets
and successors before changing their state.

**Boundary:** this claims host-side fence records only. It does not prove a
provider allocated the advertised GPU memory, authorize a tenant, reserve
an external device queue, or launch any kernel. The device adapter still
needs authenticated allocation capabilities, separate per-tenant limits,
provider-side admission, actual device completion, and physical quiescent
release. A completed preflight alone is never launch authorization.

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

## Native ownership race audit (follow-on)

Native `gpu::BufferFence` and `dop::HostBuffer` now use a wrapper-level
mutex in addition to the underlying state lock. This prevents a concurrent
`take()`/C++ move from changing `state_` after another operation has checked
it but before that operation dereferences it. Cross-wrapper move assignment
rejects overwriting a still-live allocation. Host `layout()` returns the
small checked DOP plan by **value**, not by a potentially dangling reference.

Regression coverage: GPU snapshot-versus-take and DOP read-loan-versus-take,
plus active-owner move assignment. The affected exact GitHub blobs and all
three modified C++ regression test blobs were independently recreated by
Git object SHA and compiled with Clang 17 `-std=c++17 -Werror` under
AddressSanitizer/UndefinedBehaviorSanitizer. Tests passed locally. This is
**targeted native verification**, not full LLVM, Graal, ThreadSanitizer,
real OpenCL device, or complete integration CI verification. CI jobs in the
private source organization remain unable to execute steps.

The preflight is still only a snapshot. A trusted provider must atomically
claim both physical device allocations and their fences before dispatch;
it cannot treat the returned descriptor as a security capability.

## Exact-submission completion and exception safety

**Security invariant:** generation/sequence values are diagnostic, not
authorization. A completion must present the ticket issued for that exact
allocation **and** submission. Java uses an inaccessible ticket constructor
and compares the active ticket by reference identity. Native now uses a
private, per-submission ref-counted seal embedded in the ticket; a foreign
ticket with identical numerical generation and sequence cannot complete or
cancel this allocation's operation. The native seal is created *before*
marking the allocation IN_FLIGHT and is retained across cancellation until
the backend acknowledges device quiescence.

**Failure atomicity:** both device ticket objects, identity copies, and the
paired response must be constructed before either allocation becomes
IN_FLIGHT. Java additionally allocates its typed `ClaimedBinding` wrapper
before calling the paired fence claim; assigning the issued ticket reference
afterward cannot throw. Native enforces `noexcept` moves for both its typed
binding and ticket pair at compile time. Rejected admissions leave both
allocations idle, rather than issuing a partially claimed pair.

**Locking:** Java's per-allocation State monitor protects all state, including
the moved-owner bit. The paired claim acquires two State monitors in stable
allocation-rank order, without taking an independent wrapper lock. Native
serializes owner movement with State access and acquires both owner and
allocation locks in deterministic order. Neither implementation blocks a
carrier thread on a physical GPU completion: these are only short metadata
transactions.

**Cancellation:** a poisoned allocation cannot be moved or reused. Each
ticket must reach its own authenticated completion/acknowledgment; releasing
one of a pair is *not* evidence that the other is quiescent. The future
device-provider/actor operation must coordinate both events, bytes quotas,
and physical release before publishing an owned output.

**Targeted verification:** exact-source Java fence compiled with `javac`
and passed 1,000 reversed-order claim races plus 1,000 claim/take races in
a standalone same-package harness. Exact-source native fence and regression
test blobs were matched by Git SHA, compiled with Clang C++17
`-Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined`, and
the test passed. These tests are not the complete Java DOP/kernel JUnit
suite, LLVM native CTest matrix, ThreadSanitizer, or real device execution.
