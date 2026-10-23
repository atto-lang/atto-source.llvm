# Stack-first request storage: bounded native transport slots

Status: host-only LLVM memory substrate and Linux TCP integration test.
NOT yet Oreslang guest HttpRequest/TcpRequest lowering, an HTTP parser,
a physical isolated-actor endpoint transfer, or Java/GraalVM parity.

## Contract

- Transport constructs RequestSlotTable<Slots, MaxBytes> once; fixed-size
  buffers/metadata are preallocated in transport-owned host memory.
  The bytes are NOT literally on the native call stack.
- begin(supervisor_owner) reserves a writable slot and returns a move-only
  ingress lease. Dropping it without publishing reclaims that slot.
- publish(size) freezes the bytes and produces an eight-byte value handle,
  suitable for stack/register placement. The opaque value is NOT an
  authentication token or unforgeable capability.
- transfer(handle, from, to) changes the owner and generation without
  copying request bytes; outstanding borrowed readers block transfer.
  Runtime-controlled owner identity is mandatory at all entrypoints.
- borrow(handle, current_owner) grants a scoped, immutable read view.
  Read-view pointers must not escape a source lexical lifetime, cross await,
  be stored in actor state, or be sent to other actors. C++ alone cannot
  establish the eventual guest source language proof.
- close revokes new borrows, but active readers pin the bytes until their
  leases end. Generation exhaustion retires the slot without wrapping.
  Transport teardown must quiesce all ingress, loans, and live handles.
- A mutex protects bounded slot admission/loan accounting. Published
  bytes remain immutable until safe reclamation. No C++ new/new[] is
  required per request after table construction.

## Evidence limits

The CTest native_network_request_slot_borrow_host_only includes stale
handle/owner rejection, transfer while borrowed, deferred cancellation
reclamation, abandonment, capacity exhaustion, concurrent cancellation,
and 25,000 short request cycles with no instrumented C++ new/new[].
On Linux it additionally reads a real loopback TCP request into table
ingress storage and transfers a handle across trusted owner IDs.

That is NOT proof of zero process-wide malloc, zero kernel-copy,
end-to-end guest zero allocations, production HTTP parsing, isolated
process ownership transfer, cross-backend parity, or hard real-time
suitability.

## Remaining release gates

1. Authenticate current actor incarnation and request identity in runtime
   internals, never by trusting an owner number supplied by guest code.
2. Lower guest non-sendable request handles and lexical no-escape/no-suspend
   borrowed views; preserve single-actor execution and the one-state-field
   model. Long-lived handles may survive suspend in runtime continuation
   storage without copying bytes into an actor heap.
3. Enforce bounded partial TCP reads, HTTP framing, header/body limits,
   cancellation, deadlines and backpressure before production integration.
4. Use IPC/serialized endpoints for isolated/untrusted process boundaries,
   not an in-process host pointer or shared native buffer.
5. Benchmark host and guest heap allocations separately, copies per request,
   throughput, tail latency, and peak transport-buffer occupancy.
6. GraalVM currently uses JDK HttpExchange and Java host State objects:
   host-object allocations remain until a separately verified bridge lands.
