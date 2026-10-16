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
define transform-pipe actor Dispatcher as
  async receive(ActorMail<Event> mail): void {
    // Broadcast one immutable event to every connected consumer:
    const batch = await self.forward_all(rt share mail.value);
    // Partial failure remains visible in batch.per_target.

    // Spread *other* messages deterministically:
    const receipt = await self.forward_round_robin(rt copy mail.value);

    // Affinity and explicit destination are separate routing choices:
    await self.forward_by_key(mail.value.user_id, rt copy mail.value);
    await self.forward_to("audit", rt copy mail.value);
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
- `write_round_robin_async(source, payload)` — source-local strict RR;
- `write_to_async(source, target, payload)` — authorized connected edge;
- `write_by_key_async(source, key, payload)` — stable FNV-1a edge selection.

These APIs are **host-only** and currently use `std::string` payloads;
they are not a substitute for checked `ActorMail<T>` serialization,
isolate memory ownership, or guest compiler lowering.

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
