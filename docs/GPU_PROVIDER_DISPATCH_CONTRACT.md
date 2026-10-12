# Oreslang GPU provider dispatch contract — P2 integration gate

Status: **specification only; no real device provider is enabled.** Java and
LLVM currently implement checked DOP plans, typed OpenCL source emission,
atomic GPU fence admission, and atomic paired terminal transitions. They do
not allocate physical GPU buffers, enqueue GPU work, or attest device events.
Without an authenticated provider the compiler/runtime MUST fail closed. In
particular, OpenCL code emission is not evidence that work ran on a GPU.

## Security boundary

A future trusted device provider (outside Oreslang guest interop) owns:

1. **Opaque physical allocation capabilities.** An allocation must be created
   by the selected provider, bound to an authenticated tenant, physical device,
   non-recycled generation and immutable DOP layout. Neither a user-supplied
   generation/length nor a fabricated `GpuBufferFence` is physical provenance.
   The physical handle never crosses guest or actor serialization boundaries.
2. **Provider-side quotas.** Enforce per-tenant/device resident bytes, pinned
   host bytes, outstanding events and queue slots. Reserve quota before
   submission with checked arithmetic; release resident-byte quota only after
   physical allocation release. Fence byte counts are *not* quota accounting.
3. **Submission ownership.** Keep both provider allocation handles and their
   fence owner wrappers alive throughout compilation, upload, enqueue, device
   event, download, cancellation and release. A DOP preflight snapshot never
   authorizes a GPU launch.
4. **Authenticated events.** A completion/acknowledgment must be tied to the
   actual provider event and exact submission. A Java object-identity ticket
   or native sealed ticket is a host ledger capability, not device evidence.
5. **Asynchronous actor integration.** No actor carrier thread waits for an
   OpenCL operation. Completion schedules an actor mailbox message/continuation
   through the actor's exclusive execution gate; a provider callback must not
   enter actor guest code directly.

## Required dispatch state machine

The provider must keep an independent, non-guest-accessible operation record.

| State | Permitted transition | Evidence |
| --- | --- | --- |
| PLANNED | ALLOCATIONS_AUTHENTICATED | Provider-owned physical buffer handles, device/tenant/layout identity |
| ALLOCATIONS_AUTHENTICATED | QUOTA_HELD | Atomic quota reservation for device + pinned host bytes + queue event |
| QUOTA_HELD | FENCES_CLAIMED | Atomic two-buffer fence tickets; input/output both in flight |
| FENCES_CLAIMED | ENQUEUED | Provider has durably enqueued the command and retained native event |
| ENQUEUED | DEVICE_DONE | Matching native event confirms successful or failed completion |
| DEVICE_DONE | FENCES_COMPLETED | Atomic paired terminal API commits both status changes |
| FENCES_COMPLETED | OUTPUT_RELEASED | Safe transfer/publish of verified output; eventual physical release |
| ENQUEUED | CANCEL_REQUESTED | Both buffers marked poisoned; cancellation does **not** imply quiescence |
| CANCEL_REQUESTED | CANCEL_EVENT_DONE | Both relevant native events confirmed quiescent |
| CANCEL_EVENT_DONE | CANCEL_ACKNOWLEDGED | Atomic paired cancellation acknowledgment; then handle retirement |
| QUOTA_HELD / FENCES_CLAIMED | ABORT_BEFORE_ENQUEUE | Only with a provider-proved no-enqueue witness, never by pretending completion |

**Known missing primitive:** after `claimPair` / `claim_pair`, the existing
fence ledger has no separately authenticated *pre-enqueue abort* operation.
If backend admission fails before any command was submitted, the provider
MUST NOT call ordinary completion as a substitute for real device evidence.
The implementation must add an unforgeable submission-state witness and
abort path, or redesign admission into RESERVED -> ENQUEUED. Until then,
provider dispatch remains disabled. This is an intentional fail-closed gate.

For paired work, legacy per-buffer `complete`, `cancel`, and cancelled-event
acknowledgment APIs reject while the allocation is paired. Only atomic paired
terminal APIs may finish a paired submission. All tenant/device, exact layout,
generation, sequence, and completion-seal checks remain enforced.

## DOP, source execution domains, and ABI

- Use a single typed DOP layout plan (AoS, SoA, AoSoA), including padded
  length, scalar width, alignment and lane count, for host transfers and GPU
  kernel offsets. Identical byte lengths are not layout compatibility.
- GPU kernel generation may not silently select host execution. Unsupported
  GPU hardware, 32-bit device pointers, missing f32 or excessive storage/rows
  fail with explicit diagnostics. Host ABI and endian compatibility require
  explicit proof on the actual provider.
- `cpu { gpu { ... } cpu { ... } }` requires transfer/continuation boundaries:
  the inner CPU region resumes **after** the necessary device event and
  materialization, not as a CPU callback inside the kernel.
- Actor cancellation is a request, not evidence of physical termination.
  Device memory is not reclaimed until the exact provider event is quiescent.

## Release gates

- Exact-source Java 21 JUnit and native C++17 ASan/UBSan runs using checked
  original Git blobs (no private-to-public source mirroring).
- Deterministic races: opposite-order admission, claim vs owner transfer,
  repeated/foreign/stale tickets, paired completion vs cancellation, unknown
  generation, overflow, failure before enqueue, failure after enqueue,
  timeout, and cancellation awaiting events.
- Provider integration against an actual GPU: upload, kernel, download,
  bytes/layout parity, native events, physical retirement, tenant quotas
  and actor-exclusive nonparking resumption.
- Cross-backend golden vectors and negative source conformance before
  accepting nested CPU/GPU execution-region syntax.
