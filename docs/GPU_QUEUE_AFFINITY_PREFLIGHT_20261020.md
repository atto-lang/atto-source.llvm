# GPU queue locality preflight — trusted-host safety boundary

Status: 2026-10-20. This adds **metadata-only** queue placement validation
before GPU buffer-fence admission. It does not implement OpenCL/CUDA/Vulkan
driver dispatch, actual GPU queue capacity, verified queue generation,
CPU/GPU transfers, Task/actor-to-GPU affinity, or source `gpu` syntax.

## Invariants

1. A typed DOP launch's input/output buffers must have matching tenant,
   device and complete AoS/SoA/AoSoA layout, with distinct allocation
   generations and idle fences (existing checks).
2. A selected queue description must have a nonempty tenant/device, a
   nonzero queue ID/generation and a positive declared maximum in-flight
   limit. The queue's tenant/device must match BOTH buffer allocations.
3. `claim_gpu_for_queue` runs all checks BEFORE claiming either buffer.
   A mismatch cannot leave a one-sided or partially consumed GPU fence.
4. After successful paired host-fence claim, actual completion/cancellation
   requires the existing generation/ticket/seal and provider-acknowledged
   device quiescence. The host metadata is not a hardware fence.

## Important proof boundaries

`GpuQueueLocation` contains ordinary host values. Even a matching nonzero
queue generation is not evidence of current queue ownership. A trusted
provider MUST authenticate a genuine provider-owned queue handle against
the current device generation, tenant quotas, actual capacity and in-flight
device events as one atomic physical submission operation. Host preflight
cannot reserve those things, and its `maximum_in_flight` is only validation
metadata, never a semaphore or authorization to submit.

Do not construct these descriptors from untrusted actor messages. A guest
must not be allowed to request a raw GPU queue, forge a tenant, retain an
expired device allocation, reuse host aliases, or bypass explicit upload
and download. CPU-locality hints in Java PR #1500 do not establish device
affinity or share actor-state pointers.

## Targeted regression

The updated `tests/dop_gpu_admission_test.cpp` now includes queue-targeted
same-device admission and wrong-tenant, wrong-device, zero-queue-ID,
zero-generation, zero-limit and empty-identity rejection. All failures
must leave both existing buffers IDLE. Successful claim must put both
fences IN_FLIGHT and permit exact-pair completion.

Run the full existing target `dop_gpu_admission_test` through CMake/CTest
with GCC/Clang, ASan/UBSan and stepful exact-tree source evidence.
No physical GPU performance or driver execution is claimed by this test.

## October 20 immutable receipt hardening

The queue-targeted claim now returns a `QueueClaimedGpuBinding` containing
both original paired-fence tickets and an immutable deep copy of the selected
queue descriptor. A mutable caller-held descriptor cannot retarget an
already-claimed operation. The queue receipt allocation and string copy
happen **before** both buffer fences enter `IN_FLIGHT`; after admission only
`noexcept` moves construct the return value, preserving failure atomicity.
This receipt is still host *metadata*, not a hardware queue capability.

## Immutable handle audit

The queue receipt now exposes the selected target through `queue() const`
only. The receipt owns a private immutable descriptor; caller code cannot
replace its pointer or assign a different queued receipt after the two
buffer fences have been admitted. The moved-from receipt's `queue()` accessor
throws rather than dereference an empty owner. Source metadata is copied
before admission; device/provider authentication remains a separate gate.

## Constructor and ticket sealing

Only `claim_gpu_for_queue(...)` is a friend of the queue-scoped receipt's
private constructor. A host caller cannot fabricate a `QueueClaimedGpuBinding`
by attaching an arbitrary queue descriptor to tickets produced by the
queue-neutral API. The admitted `ClaimedGpuBinding` and its paired fence
tickets are exposed read-only through `claim() const`, and queue selection
through `queue() const`. A moved-from receipt rejects either accessor.

This is a host-level compatibility invariant, **not** physical device
attestation or proof of queue generation freshness: the GPU provider must
still verify the actual device handle, tenant, generation and capacity before
submission, and retain fences through authenticated completion.
