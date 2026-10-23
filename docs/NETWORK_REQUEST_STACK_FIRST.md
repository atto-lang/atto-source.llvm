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

## HTTP request-head borrow substrate

HttpRequestHeadView parses a bounded, strict ASCII subset of HTTP/1.0 and
HTTP/1.1 request-line and header syntax directly from an active ReadLease
string_view. It exposes borrowed method, target, version and header values
without allocating a request object or header map. The 32 KiB metadata bound
and 100 header count bound fail closed. It rejects whitespace-prefixed
obs-fold, C0/DEL value controls, malformed names and incomplete head blocks.

This is deliberately NOT a full HTTP server parser: no body framing, content
length reconciliation, transfer-encoding checks, TLS, HTTP/2, trailer, or
request-smuggling validation is included. In particular, first_header()
is not a safe way to adjudicate duplicate Content-Length or Transfer-Encoding
fields. Real production ingress must use a complete framing policy first.

## Evidence limits

The CTest native_network_request_slot_borrow_host_only includes stale
handle/owner rejection, transfer while borrowed, deferred cancellation
reclamation, abandonment, capacity exhaustion, concurrent cancellation,
and 25,000 short request cycles with no instrumented C++ new/new[].
The additional borrowed HTTP head test exercises 10,000 request/parse/close
cycles without instrumented C++ allocations plus malformed-header rejection.
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

## Host-only actor-incarnation binding (second slice)

ActorBoundRequestTable<ActorCapacity, Slots, MaxBytes> adds a bounded,
preallocated actor-authority directory over the request slots. The trusted
runtime activates an (actor-id, incarnation) pair and receives a distinct,
monotonically minted internal owner token. Reusing the actor ID with an old
incarnation never restores its request access. The directory serializes
begin/borrow/transfer/close with actor retirement; the retirement path calls
RequestSlotTable.revoke_owner before reusing an actor directory entry.

Retirement revokes published handles immediately but keeps outstanding read
leases pinned until they finish. If retirement catches an unfinished ingress,
the slot enters an aborting phase: publish refuses the write, while storage
stays valid until the owning ingress lease is dropped. Generation and owner
tokens never wrap into a previously valid authority. Native scoped callbacks
return their leases on both normal exit and C++ exception unwinding.

This is still a HOST reference: ActorIncarnation inputs must eventually come
from runtime-authenticated execution/CONTROL state, never user-supplied
messages, generic ints or unauthenticated RPC. The adapter is not connected
to compiled ActorMail source dispatch. Exposing the underlying raw slot table
to guest code would violate that trust boundary. Cross-process isolated and
untrusted workers require their separate protected broker/IPC protocol.

The test native_actor_request_incarnation_revocation_host_only exercises
retirement, stale generations, in-progress writer cancellation, deferred
reclamation, owner handoff, 25k allocation-instrumented scoped cycles, and
native exception cleanup. GCC/Clang ASan+UBSan local harness passes do NOT
satisfy exact-head hosted CTest or guest-language execution gates.

## Security audit and hardening (October 23)

The host-only actor authority directory previously forgot an actor's old
generation when the directory entry retired. It now retains a bounded,
non-evicting high-water generation record per distinct actor ID.
Every subsequent activate(id, generation) must increase that generation
strictly. When the history budget fills, activation for a *new* ID fails
closed rather than evicting identity history. HistoryCapacity is a configurable
template parameter; default ActorCapacity * 64. This is a deliberate bounded
host proof, not a production long-lived actor-registry sizing policy.

Request-slot free-ring invariants, reader-count underflow and begin-generation
checks are now enforced via std::terminate rather than debug-only assert. Bad
internal bookkeeping cannot silently corrupt the queue in release builds.

HttpRequestHeadView now has safe_unframed_request(), an intentionally
restricted *whole-buffer* admission gate that permits only complete, bodyless
HTTP/1 GET/HEAD metadata. It rejects ambiguous/multiple Host, noncanonical or
duplicate Content-Length, Transfer-Encoding, Expect, Upgrade, pipelined bytes,
extra body bytes and proxy-ambiguous Host spellings. It is NOT a general
RFC 9112 framing implementation or a body/streaming/HTTP/2/TLS parser. The
existing parse() and first_header() remain metadata-only and must not be used
to decide framing on production ingress.

Host-side regression probes additionally cover replaying an old actor
generation, monotonically restarting the same actor ID, exhausting the
fixed identity-history budget without eviction, 256 concurrent borrow vs
retire races, and malformed unframed HTTP request-head cases.

IMPORTANT unresolved integration boundary: RequestSlotTable's 8-byte Handle
only encodes (slot, generation), NOT transport-table provenance.
ActorBoundRequestTable's numeric ActorIncarnation is also NOT an unforgeable
guest capability. A future dispatcher must bind message routing to the
particular transport-table/process epoch and obtain actor identity from
trusted runtime execution context; it must not resolve arbitrary user-supplied
handles into arbitrary tables. Table destruction/recreation and process
restart replay MUST be tested once that binding exists. No public guest
API or deployment should bypass that gate.

Local compiler/sanitizer harnesses are not the exact GitHub PR-head CTest or
JDK25/Native Image/compiled guest evidence required to advance from draft.
