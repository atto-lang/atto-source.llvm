# Oreslang Sendable / Serializable boundary contract (v1 proposal)

Status: **proposed migration contract, NOT implemented language syntax or evidence of backend parity** (2026-10-15). Applies to Java/GraalVM, LLVM, .NET, Wasm, Forge and shared conformance; see the identical [five-backend parity gate](../../contracts/v1/SENDABLE_SERIALIZABLE_PARITY_V1.md) and [admission fixture](../../contracts/v1/sendable-serializable-parity.v1.json). It deliberately separates local actor transport from network encoding. Until a compiler/runtime path is certified under this contract, its existing stricter MessagePack actor-transport gate remains in force.

## Two independent questions

* `Sendable` asks whether an owned value can **safely leave its current isolation domain** and be delivered to a different actor or supervisor without exposing mutable aliases, live borrows or unrevocable resource access.
* `Serializable<Format>` asks whether a value has a **statically resolved, bounded, schema-governed encoder and decoder** for a specific wire format. It must also be `Sendable`; implementing or annotating a method called `serialize` alone is not proof.
* `Sendable` **does not imply** `Serializable<MessagePack>`, `Serializable<Json>` or `Serializable<Protobuf>`. Codec derivation is opt-in and independently verified per format; no universal reflective fallback or implicit `toString`.
* A local `ActorRef<T>` or supervised CONTROL capability may be transferable **only as an authenticated, scoped runtime capability**, not as serializable data. A user class, OS handle or raw pointer is never admitted merely because it is recursively made of primitive fields.
* Network decoders produce *untrusted typed data*, never executable code, actors, resources, synchronization objects, or authority. A network address/actor identifier is data, **not** a reconstructed `ActorRef`. Remote actor routing requires an explicit authenticated bridge and policy decision.

Proposed source spelling (illustrative; syntax not yet implemented):

```ores
// Static/compiler-derived, not a reflection-time unchecked assertion.
define struct Event derives Sendable, Serializable<MessagePack>,
    Serializable<Json>, Serializable<Protobuf> as
  id: string
  count: int
end

// A local capability may be Sendable without any wire codec.
define actor Supervisor as
  receive(ActorMail<Event> mail): void { ... }
end
```

The compiler should infer `Sendable` only for closed, completely initialized, owned data graphs whose transitive constituents have proven transfer rules. Explicit implementations must not override compiler-known negative types. Manual wire codecs require a compiler-registered descriptor and verification; `@Serializable` alone confers no privilege. All generic `T` must carry and satisfy explicit bounds after import/substitution. Absence of a proof means rejection, not an unsafe dynamic fallback.

## Boundary table

| Boundary | Required proof | Representation | Capability policy |
| --- | --- | --- | --- |
| Supervisor to child; child to supervisor, same trusted runtime | `Sendable` + recipient type + provenance | Owned move, immutable copy/snapshot, or proven safe arena transfer | CONTROL lifecycle/supervision events use a separate privileged envelope; local scoped references must be checked against sender, target, runtime, actor group, and generation |
| Actor to actor, separate heaps within one process | `Sendable` + receiver mailbox type | No writable alias to sender; move or deep frozen snapshot. Java may use MessagePack as an interim transport | Capabilities admitted only by explicit local transport policy; never serialized |
| Actor to actor across processes/hosts | `Sendable` + explicit `Serializable<Format>` + trusted typed schema | Length-delimited, quota-accounted encoded bytes and receiver-owned decode | No implicit ActorRef/capability export; authenticate bridge and generation separately |
| TCP/HTTP/WebSocket/file/network send | `Serializable<ChosenFormat>` and sendable ownership proof | Explicit encoder, Content-Type/format, limits and version | Data-only; no reflection or authority reconstruction |
| TCP/HTTP/WebSocket ingress | Independently configured `Deserializer<ChosenFormat,T>` | Reject malformed/ambiguous input before creating a trusted message value | Decode must never grant actor/supervisor capabilities |

**Legacy warning.** The Java source actor MessagePack v2 mailbox codec and native MessagePack v2 subset predate this split. Do not silently change wire bytes, remove existing quotas, or claim that LLVM can currently lower general local `Sendable` objects. Unimplemented optimized move paths must keep the existing safe serialize/snapshot path or fail closed.

## Common invariants

1. **Publication is atomic.** Validate full transitive graph and type/schema, acquire receiver+runtime byte/node quotas, snapshot/move or encode, and only then publish one mailbox entry. Failures, cancellation, timeouts, actor death and supervisor restart release **all** reservations and owned storage exactly once. Never invalidate sender on failed enqueue; successful move invalidates sender before any concurrent reuse.
2. **No alias leakage.** Reject nonstatic/live borrows, closures, executable functions, mutable sender aliases, `Channel`, `Future`, `Mutex`/guard, raw pointers, host objects, inherited mutable fields, and unknown/cyclic opaque graph types. Shared immutable graphs require transitive frozen proof; class field visibility does not grant transfer safety.
3. **Recipient and provenance.** The receiver's **trusted expected type/descriptor** chooses the decoder, not a type string or hash supplied on the wire. Check actor identity, generation, tenant, group membership, mailbox domain and permissions on admission; supervision CONTROL metadata never travels inside untrusted data payload.
4. **Bounded adversarial parsing.** Enforce a byte/frame limit *before* allocating, depth/node/container limits, strict UTF-8/Unicode, length overflow checks, duplicate-field/key policy, recursion control, canonical primitive shape, and full-input consumption. Malformed input cannot partially mutate actor state or run constructors with side effects.
5. **Schema identity and evolution.** Compiler-owned canonical identities include module/package, major schema version, full type instantiation, exact numerical width/signedness, collection kind, field tags and nominal enum variants. Unknown major versions, tags or incompatible fields fail closed; version migrations require explicit reviewed adapters. Never reuse a tag for a different meaning.
6. **Data fidelity.** No silent numeric truncation/coercion, `NaN`/infinity propagation unless a specific codec explicitly supports them, time-zone conversion, arbitrary map key stringification, or accidental distinction loss between `Array`, `List`, tuple, record and named enum.
7. **Streaming and scheduling.** Encoding/decoding must obey CPU budgets, bounded allocations and cooperative cancellation. Hard-real-time LLVM profiles additionally need bounded worst-case encoding time/storage or must reject network/codec operations inside hard-RT turns.

## Format adapters

* **MessagePack:** Keep existing Oreslang actor wire **v2** byte vectors stable: `[2, fq_schema_id, bin8(32, SHA256("oreslang.actor.shape.v2/" + shape)), schema_guided_payload]` for its supported subset. Shape hash prevents accidental mismatches but is **not** authentication. Distinguish signedness, widths, array/list and nominal tags before expanding the subset. Network framing, sender auth and replay defense are separate layers.
* **JSON:** Use UTF-8, strict parser and duplicate-key rejection, explicit schema/field names, depth and byte quotas, and an exact documented policy for integers beyond the interoperable IEEE-754 range, decimals, binary data, enum discriminators, `null`/`Option`, non-finite floats and map keys. Reject lossy mappings unless an explicit schema adapter opts into them. Object key order must not carry semantic meaning.
* **Protobuf:** Require a generated descriptor/known `.proto` schema and field numbers. Preserve unknown fields only under an explicit retention policy and bounded quota. Reject illegal wire types, repeated scalar ambiguity, duplicate singular fields per chosen policy, numeric range violations and incompatible `oneof` tags. Explicitly document Proto3 presence/default distinctions and deterministic encoding when comparing bytes. Protobuf encoding alone does not authenticate the sender.

## Conformance and rollout gates

**P0 / compiler:** Define a compiler-owned `Sendable` proof separate from `Serializable<Format>`, including transitive generic substitutions, recursive nominal types and negative capability types. Reject unknown nominal types, fake derives, malformed generic arity and typed `ActorRef` targets. Audit Java source type checking and LLVM ownership lowering independently.

**P1 / local runtime:** Prove same-runtime supervisor↔child and actor↔actor owned move, copy and zero-alias snapshot, generation checks, shutdown/cancellation and quota release. Demonstrate that a locally sendable value without a wire codec is accepted only after the local transfer path is implemented; retain MessagePack fallback in the meantime.

**P2 / wire runtime:** Integrate separately registered MessagePack/JSON/Protobuf codecs at *explicit* network I/O APIs, with decoder-selected descriptors, per-connection limits, authenticated remote proxy routing, and negative fuzz corpus.

**P3 / parity:** Run the same positive/negative fixture manifest on Java/JUnit, Graal native-image and LLVM/CTest/sanitizers; compare MessagePack exact bytes, JSON semantic round trips with canonical test policy, Protobuf descriptor evolution and malicious input. Test every transport under lifecycle races and multi-tenant quotas. Do not call standalone codec vector success an end-to-end actor delivery pass.

**Release must remain blocked** on any unproved transfer semantics, unsafe capability serialization, import/generic proof gaps, or LLVM actor send/receive lowering gaps. Older transport audits that say “every actor message requires Serializable” describe the legacy binary-only implementation, not the target `Sendable`-first design.
