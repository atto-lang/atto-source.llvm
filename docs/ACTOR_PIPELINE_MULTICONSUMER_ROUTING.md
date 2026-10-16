# Actor pipeline routing API — pre-1.0 contract

Status: **host routing substrate implemented in LLVM** (checked graph and
bounded mailbox admission), **source-language `self.*` lowering NOT yet
implemented**. This document defines the language contract for Java/GraalVM
and LLVM. Do not market its source examples as executable or turn arbitrary
actor methods into implicit network capabilities.

## Actor-facing built-ins (pipeline actors only)

The following are sealed, VM/scheduler-owned *special actor methods*. They
must not be user-overridable and must reject ordinary classes, non-pipeline
actors, or actors with no authorized connected port:

| Method | Semantics | Return contract |
|---|---|---|
| `self.forward(payload)` | One downstream; **rejects ambiguous fan-out topology** | `Future<RouteReceipt>` |
| `self.forward_all(payload)` | One independent copy/immutable projection per connected downstream | `Future<RouteBatchReport>` |
| `self.forward_round_robin(payload)` | Exactly one connected downstream per call, strict per-source cursor | `Future<RouteReceipt>` |
| `self.forward_to(target, payload)` | One named, typed, authorized connected downstream port/actor | `Future<RouteReceipt>` |
| `self.forward_by_key(key, payload)` | One stable key-affinity destination for current graph generation | `Future<RouteReceipt>` |
| `self.backward(payload)` | Reply along current upstream correlation, not arbitrary reverse broadcast | `Future<RouteReceipt>` |
| `self.reply(payload)` | Reply to authenticated origin of currently handled request, including fan-in | `Future<RouteReceipt>` |

`forward_all` is the canonical *fan-out/broadcast* operation.
The native host's `write_all_report_async` produces explicit ordered
`{edge_index, target, admission}` entries, so callers can identify successful
and failed branches without inferring a destination from vector position.
The compatibility `write_all_async` API remains available, but its unlabeled
futures are not the future guest-facing `RouteBatchReport` representation.
`forward_round_robin` is canonical *load spread*, not "send to all."
`forward_by_key` supports partition locality. Avoid introducing
synonymous `broadcast`, `fanout`, `scatter`, `send_all` variants until
the typed surface and formatter have stabilized. A future `forward_available`
may select based on credits, but must not introduce silent nondeterminism
into current round-robin semantics.

## Exact operational rules

1. **Connection authority and topology.** The supervisor validates every
   edge before publishing the pipeline. Source and destination IDs/epochs are
   authenticated, every link has an exact nominal payload type, no duplicate
   links or dangling ports are allowed, and every graph is bounded/acyclic.
   **An actor instance ID is not its actor class/type**: multiple Worker
   instances can subscribe to the same Producer. Native `PipeStage.instance_id`
   identifies graph vertices; an omitted instance ID uses the nominal type
   only for backward compatibility. In production the supervisor must bind
   these identifiers to authenticated runtime actor identities/epochs.
   No caller-supplied actor name grants authority to another actor's port.
2. **Single physical execution.** Routing-policy mutation (RR cursor,
   subscription membership, reply correlation) belongs to the VM scheduler,
   **not** a guest-writable global or hidden ordinary actor instance field.
   An actor never runs on two carriers simultaneously even if many logical
   messages await admission.
3. **Ownership.** `forward_all` requires a copyable/serializable or deeply
   immutable payload; each destination receives its own ownership domain.
   Sending one moved noncopyable value to multiple consumers is a compile
   error; no cross-actor mutable aliasing or pointer/ActorRef escape.
   `rt take` and `rt borrow` loans may not cross a suspended route operation.
4. **Bounded backpressure.** Every outbound edge has byte, message and
   pending-writer credits. Routing never blocks a native carrier, retries
   implicitly, or invents unbounded buffering. All receipt Futures settle on
   authorized scheduler continuation lanes; they do not run user code inline.
5. **Admission versus execution.** `RouteReceipt` proves *only* mailbox
   admission, not that the target processed or committed the message.
   Exactly-once processing is NOT implied. Receipt cancellation does not
   undo an already admitted message.
6. **Fan-out partial failure.** Independent edge admission may succeed for
   some destinations and fail or remain backpressured for others. The
   `RouteBatchReport` MUST contain one stable per-destination result.
   Do not misreport overall success if any branch fails. A truly atomic
   fan-out API would need separate all-edges credit reservation and rollback.
7. **Round-robin.** Cursor is scoped per source actor, outbound connection
   group and topology generation. Order is the canonical supervisor-approved
   edge list. Each attempted route consumes one turn—even if admission
   later fails or suspends. It does not silently skip congested consumers.
   RR state must be generation-fenced on membership changes.
8. **Key affinity.** v0 uses FNV-1a 64-bit over a bounded canonical UTF-8 key,
   modulo the ordered destination count. This is not a security hash.
   Reconfiguration changes the partition map; callers must coordinate a
   graph-generation barrier if stable placement across topology updates matters.
9. **STOP/cancellation.** STOP seals new user-route admissions. Already
   admitted envelopes may drain according to the actor's terminal policy,
   while pending writes settle with cancellation. Terminal cleanup, release,
   output closure and DONE remain physically ordered; no `await` inside
   borrowed actor-state callback or across terminal release.
10. **Backwards/fan-in.** `backward` is a **correlated reply** to the
    immediate authenticated origin, not a guess based on a single global
    "previous actor" slot. Multiple upstream producers require per-message
    correlation ID/return path, authenticated origin, and epoch/generation.
    If an explicit upstream multicast is needed later, it requires its own
    typed reverse-outbound graph and distinct authority checks.

## Source shape — future syntax, not yet accepted

```ores
// Illustrative ONLY until the Java and LLVM compiler lowering gates pass.
define transform pipe actor Dispatcher as
  forward_messages: Event
  backward_messages: Ack

  async receive_forward(ActorMail<Event> mail): void {
    // Broadcast one immutable event to every connected consumer:
    const batch = await self.forward_all(rt share mail.value);
    // Partial failure remains visible in batch.per_target.

    // Spread *other* messages deterministically:
    const receipt = await self.forward_round_robin(rt copy mail.value);

    // Affinity and explicit destination are separate routing choices:
    await self.forward_by_key(mail.value.user_id, rt copy mail.value);
    await self.forward_to("audit", rt copy mail.value);
  }

  async receive_backward(ActorMail<Ack> mail): void {
    // A real guest implementation must use authenticated flow correlation.
    await self.backward(rt copy mail.value);
  }
end
```

These statements must stay **compiler-rejected until implemented** in the
source language and JNI/LLVM scheduler integration. A typechecker-only
acceptance of these methods is not a working actor pipeline.

## Implemented trusted native host substrate

`oreslang::runtime::check_actor_pipeline_graph` validates a bounded,
acyclic, typed DAG. `NativeActorPipeTransport` accepts an explicit graph
constructor with independent bounded mailboxes and implements:

- `write_all_async(source, payload)` — ordered vector of per-edge Futures,
  non-transactional fan-out;
- `write_all_report_async(source, payload)` — individually tagged target
  receipts and their independent admission Futures;
- `write_one_async(source, payload)` — single-edge-only default forward;
  rejects ambiguous branching;
- `write_round_robin_async(source, payload)` — source-local strict RR;
- `write_to_async(source, target, payload)` — authorized connected edge;
- `write_by_key_async(source, key, payload)` — stable FNV-1a edge selection;
  rejects malformed/overlong UTF-8, surrogates and oversize keys. Unicode
  normalization (NFC), if required, belongs to the typed key codec upstream.

These APIs are **host-only** and currently use `std::string` payloads;
they are not a substitute for checked `ActorMail<T>` serialization,
isolate memory ownership, or guest compiler lowering.

### Native aggregate payload accounting (2026-10-15 hardening)

The native transport now accepts optional graph-wide **buffered byte**
and **buffered message** capacities, defaulting to **64 MiB / 65,536**
and restricted to **1..64 MiB / 1..65,536** respectively. An individual
DATA payload is limited to **1 MiB**, while the edge and supervisor worker
capacities are each restricted to **1..4096**. These are independent,
atomic pressure gates shared across *all* graph edges. A zero-byte DATA
message still consumes one message slot, so empty messages cannot evade
the resource bound.

Pending **reader** promises have a separate graph-wide cap, defaulting to
65,536 and configurable over 1..65,536. This closes the multiplicative
allocation gap of up to 4,096 edges with 4,096 pending readers each.
Each accepted pending read takes one global credit. A direct write handoff
releases the reader credit, and STOP releases all remaining pending reader
credits while failing their Futures. Per-edge reader bounds still apply.
The host exposes `pending_reader_count()` for accounting diagnostics.
Native regressions cover cross-edge saturation, credit reuse after handoff,
STOP cancellation, and invalid global reader limits.

Bytes are charged only for a buffered message or pending writer, before
publication; a failed reservation never consumes credits. Reading a queued
message releases its bytes. Promoting an already charged writer preserves
the charge until its DATA is read. STOP releases cancelled pending-writer
credits but preserves credits for previously admitted buffered DATA, which
remains drainable. A direct handoff to an already-waiting reader is not a
buffered transport allocation. The host-only `buffered_payload_bytes()` and
`buffered_message_count()` diagnostics expose the currently charged
payload bytes and buffered/pending messages.

This is **not** a global heap cap on consumers holding already-delivered
Futures or a claim of guest-level backpressure. The future scheduler
`RouteReceipt`/typed `ActorMail<T>` integration must enforce the same
generation-scoped credit policy (including accounting for actual encoded
payload sizes) on Java/GraalVM, LLVM and other backends. It must also
preserve one failure receipt per fan-out destination under byte pressure.


## Native host route-correlation ledger (2026-10-16)

`ActorPipeRouteLedger.hpp` adds a C++17 **host-only** immutable
correlation frame ledger over the validated DAG, with supervisor-selected
ingress roots, generation-scoped flow IDs, independent route IDs for each
fan-out message, one-shot frame retirement and 1..65,536-frame capacity.
The ledger checks every selected forward edge, rejects ambiguous forwarding,
preserves the published edge order, uses per-source round-robin cursors,
and rejects foreign/stale frames and graph reconfiguration with live frames.
An explicit supervisor abort invalidates outstanding frames.

A reverse `backward(frame)` selector creates exactly **one** new,
immutable, VM-host frame pointing to the immediate previous actor instance
in the verified original path. Both fan-in branches keep their distinct
paths; the original frame is consumed exactly once, the flow ID stays
constant, and the reverse hop has its own route ID. Reverse data cannot
re-enter forward selection or route above the origin.

**Frame lifetime hardening:** the ledger retains owning references
to active route frames until completion, STOP or generation abort.
Dropping the final external handle cannot leave a dangling pointer or
allow an allocator-reused address to authenticate a different frame (ABA).
The updated exact C++ header and test blobs were compiled and run locally
with GCC and Clang in C++17 ASan+UBSan mode (2,000 concurrent selections).
Those local runs still used a lightweight graph-contract stub, not the
production native graph validator or entire CTest matrix.

**Important:** this ledger is NOT yet attached to native DATA mailbox
envelopes and does not authenticate the current executing actor. It also
does not lower any Oreslang source `self.forward_*`, `self.backward` or
`self.reply` method. The Java/GraalVM draft contains a more integrated,
real-mailbox SHARED host bridge, so this constitutes **correlation-substrate
parity only, not executable backend parity**.

`native_actor_pipe_route_ledger_test.cpp` is registered with CTest and
the standalone GCC/Clang O0/O2/ASan/UBSan matrix. A local syntax and
algorithm test of the **exact new header and test Git blobs**, using a
type-compatible lightweight `ActorPipeContract.hpp` graph validator
stub, passed GCC 14 and Clang 17 under ASan/UBSan with 2,000 parallel
route selections. The production DAG validator, actual native transport,
full repository CTest and compiled guest actor code were **not executed**
by that isolated test and remain release blockers.

## Mandatory release gates

- Compile and execute the new host graph/routing C++ CTests with both
  GCC and Clang, in Debug/Release and sanitizers.
- Add matching Java admission/selection substrate and exhaustive nominal
  type/payload/ownership tests.
- Lower the sealed self methods to scheduler-managed typed Route Futures in
  Java and LLVM, with source-compiled programs (not handwritten host code).
- Execute a three-plus actor fan-out/fan-in graph with typed values and
  correlated return via `self.backward`/`self.reply`; test saturation,
  cancellation, duplicate sends, pipeline reconfiguration, actor STOP and DONE.
- Verify exact source Git tree on the authorized runner; never mirror private
  source into an unapproved external organization.
