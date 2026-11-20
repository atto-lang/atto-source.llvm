## Native tagged-message implementation update (2026-10-11)

The canonical source syntax is now one qualified case **per line**, without `|`:
```ores
define enum Status as
    Message1: Tuple[bool, string] = 1
    Message2: string = 2
end
define pattern Messages as
    Status.Message1
    Status.Message2
end
```
The Java/GraalVM compiler and source actor runtime implement this initial subset; [exact-source Maven run #38171602084](https://github.com/gha-indie-worker/gha-indie-worker-docs/actions/runs/38171602084) passed 45 JUnit tests over 79 verified blobs. **LLVM source-level enum/pattern declaration and typed actor `send`/`receive` lowering remain unimplemented**.

The native C++17 `tests/tagged_message_wire_v2_test.cpp` independently calculates the compile-time closed-domain hash, nominal case identity `Status.Message2#2`, and the canonical typed MessagePack v2 frame. It passed [funded native run #38171435534](https://github.com/gha-indie-worker/gha-indie-worker-docs/actions/runs/38171435534), comparing exact bytes with `contracts/v1/design/tagged_message_vectors.json`; optional CMake target `native_tagged_enum_message_java_wire_parity` is registered but has not undergone a full LLVM CTest suite. This is **binary codec parity only**, not complete backend/actor-runtime parity. All enum tags must be monotonically increasing positive integers; schema hash is not a sender-authentication MAC.

---

# RFC: native enum and tagged message ABI parity

**Status: proposal only; no new LLVM source behavior or test results are claimed.**

Canonical design source (Java repo):
[ENUMS_TAGGED_MESSAGES_AND_WIRE_IDENTITY.md](https://github.com/ores-truffle-oreslang/oreslang-source.java/blob/design/enum-and-tagged-actor-messages-20261011/docs/proposals/ENUMS_TAGGED_MESSAGES_AND_WIRE_IDENTITY.md).

## Native requirements

Oreslang should support two **separate declaration kinds**: `define pattern P as A | B | ... end` is a transparent, type-only domain with **no constructors or values**, and `define enum E as ... end` is a nominal type with a namespace of constructible named case values. A pattern can include an enum type, but an enum (including payload types through imported aliases, nested generics, or transitive references) cannot reference any named pattern. Existing `type T = A | B` aliases continue to work and must not erase the source declaration kind used to enforce that prohibition. `match` is an expression/statement that tests values, not a type declaration. A match expression may infer its result or constrain its result with `match<R>`; no per-pattern result annotation is needed. `do match` is statement-only. Integrate with the existing `over/on` proposal and do not unilaterally change match ordering/exclusivity during the enum work.

Proposed source fixture (NOT admitted by current LLVM source frontend):

```ores
define enum WorkerMessage as
  Start(job_id: string)
  Progress(done: int, total: int)
  Stop
end

define pattern AcceptedMessages as
    WorkerMessage
  | Tuple[bool, string]
  | string
end

// AcceptedMessages is type-only, never a runtime case namespace.
// Invalid: enum case payload of type AcceptedMessages.

define actor Worker<messages=WorkerMessage> as
  receive(WorkerMessage m): void {
    do match m over
      on WorkerMessage.Start(job_id) -> { /* ... */ }
      on WorkerMessage.Progress(done, total) -> { /* ... */ }
      on WorkerMessage.Stop -> { /* ... */ }
    end
  }
end

worker.send(WorkerMessage.Start("job-123"));

// A separately declared actor may take <messages=AcceptedMessages>.
// A pattern accepts enum *types*, not singleton values as type expressions.
```

## Type-system invariants

- `pattern` is a declaration kind, not a runtime value. Do not generate an enum-like constructor, tag, symbol, UUID, or first-class runtime object for a pattern name. A pattern domain is statically closed but alternatives like `string` have unbounded values.
- Use explicit `|` alternative separators; the current frontends generally ignore source newlines and must not infer union boundaries from layout.
- The enum declaration is a type and an **owner of case values**; it is not itself a runtime value. `WorkerMessage.Stop` is a value while `WorkerMessage` is a type.
- `pattern` may reference `enum`. Reject enum payloads that transitively depend on any named pattern, even if a `type` alias or generic wrapper obscures the reference. Reject dependency cycles.
- `on E.Case` is a constant value match; `on is E value` is a type pattern. A bare `E.Case` is **not** a type alternative in a named pattern declaration for v1.
- A pattern declaration never has a result type. `match<R>` optionally constrains the whole match expression result; `do match` is statement-only. Actor receive signatures independently specify their callback return types.

## ABI constraints

- A backend-local discriminant is an implementation detail, not a portable identifier. Runtime pointer addresses, Java objects, ELF symbol names, LLVM enum ordinals, PostgreSQL OIDs and per-process interned Symbols must not enter the durable wire contract.
- Descriptor: nominal qualified type identity plus schema contract version; each case has a stable canonical string tag, typed payload layout and compiler-checked constructor arity and ownership.
- Native representation can optimize nullary cases as compact scalars and payload cases as discriminant + sized/aligned owned payload, provided constructor/destructor behavior, actor-heap lifecycle and FFI boundaries obey the same semantic contract as Java/GraalVM.
- No implicit casting from other enums with coincident labels. Reject missing/unknown tags on decode and avoid dynamic atom/Symbol interning from untrusted data.
- `Actor<messages=T>` and `send(T)` require both static and runtime validation; actor payload copying/moving may not create cross-actor writable aliases. Dispatch and receive occur on the actor's exclusive logical scheduler lane.
- Payload serialization must not rely on layout pointer bytes or padding; define an independent canonical codec, version, tagged structured payload, field names, numeric types and evolution policy before cross-process messages are enabled.
- Decoding a payload case for a stopped/restarted actor must honor generation fencing, mailbox cancellation and reclamation. No stale pointers into previous actor heaps.

## Mandatory serializable actor-message type and wire ABI

`ActorRef<T>.send(value)` must reject any value that cannot be serialized through a compiler-known `Serializable` descriptor, **even when sender and recipient share a process or scheduler**. This is a stricter type property than sendability/read-only freezing. The actor message-domain check and the recursive `Serializable` proof are two independent required checks: `value: Domain` and `value: Serializable`.

- Compiler: recursively prove codecs for accepted primitive/scalar types, tuples, finite/homogeneous arrays and lists, closed readonly/inferred records, `Option<T>`/`Result<T,E>`, tagged enums with recursively serializable payloads, and named pattern domains whose **all** alternatives are serializable. Reject opaque/erased generics, borrowed views, closures, raw pointers, FFI handles, native class identity, mutexes, channels, futures and raw actor refs. Compile-time `Serializable` capability must not be forgeable by a cast or host descriptor.
- Derivation: the future `@Serializable` annotation for user-defined structs/classes/enums must emit schema descriptors, checked encoders and decoders plus version metadata. Merely recording the annotation or implementing a marker interface is **not** sufficient. No source-level derivation is claimed in this PR.
- ABI: use a portable, versioned schema identity and stable case tags, not LLVM layout/struct padding, host endianness or enum ordinal. Byte-for-byte cross-backend vectors must specify numeric semantics, string encoding, map key normalization, size/depth/node quotas and schema evolution. Restrict decode by recipient policy and actor generation.
- Runtime: validate and snapshot/encode before committing mailbox admission. An optimized same-process transport may elide byte materialization *only when* it proves an identical immutable decoded value and an actual portable codec exists. PRIVATE/UNTRUSTED isolation and source actor-owned heaps cannot inherit writable aliases. Cancellation, quotas, nonblocking scheduler admission and actor teardown reclaim all staged payload resources exactly once.
- Security: dynamic `Any`, JSON decoding and FFI cannot bypass static checks; unknown variant IDs/versions, cyclic object graphs, executable closures, process-local `Symbol` atoms, invalid capability tokens and oversized encoded messages fail closed.
- Capability compatibility: current JVM host actor transport has explicitly scoped exceptions for controlled actor-reference and shared handles. Do not treat those as `Serializable` user messages. Forwarding a capability through the future public `send` requires a distinct audit-approved serialized capability descriptor or migration to a separate CONTROL mechanism; no raw pointer or process-local handle travels in data mail.

Example intended source shape (not yet LLVM-admitted):

```ores
const a = await spawn WorkerActor();
a.send("foo"); // only if the declared message domain includes string

// All cases recursively serializable -> a serializable enum.
define enum Status as
  Queued = "queued"
  Done = "done"
end

// All alternatives must be Serializable, not merely Sendable.
define pattern Messages as
  string | Status | Tuple[bool, string]
end
```

**Scope note (updated):** Java/Graal now implements canonical MessagePack v2 for supported scalar/collection actor messages, including live source-level actor mailbox integration and 39 passing exact-source Maven/JUnit tests. The native LLVM draft now implements a separate C++17 canonical binary codec with six passing cross-backend golden vectors; **LLVM source-level actor lowering and enum/pattern construction/matching are still unimplemented**, and `@Serializable` derivation is still reserved. LLVM's existing fail-closed reservations remain correct while source-level lowering is absent.

## Transport decision: canonical shape-bound MessagePack v2, no implicit alias sharing

Default to **MessagePack binary serialization**, not JSON/text, for all mailbox deliveries, including same-process SHARED actors. An Oreslang compiler-owned schema/codec descriptor disambiguates `Tuple`, `FixedArray`, `FixedList`, dynamic `Array` and `List`: MessagePack stores all of them as arrays, so native LLVM must preserve nominal/structural container kind using the **expected schema**, not a guess based on decoded bytes.

The reference subset now uses **MessagePack wire v2** `[wire_version=2, schema_id_utf8, SHA256("oreslang.actor.shape.v2/" + canonical_shape) encoded as bin8(32), schema_typed_payload]`. Canonical shape grammar: `B` bool, `I` signed i64, `F` finite f64, `S` UTF-8 string, `T[...]` Tuple, `FA[...]` FixedArray, `FL[...]` FixedList, `A[T]` Array and `L[T]` List. Include exact member ordering and arity in the digest. Version 1 is deliberately rejected (no migration implied). The digest is a type-identity binder, not sender authentication. Decoder rejects noncanonical integers/string/array headers and untrusted array lengths exceeding the remaining byte/node budgets **before allocating**. The decoder validates version, schema, fixed shape/arity, scalar type, valid UTF-8, signed integer range, finite floating point values, depth/node/length/byte quotas and complete input consumption. Type descriptors are compiler-owned and cannot be substituted by untrusted payloads. `contracts/v1/design/messagepack_collection_vectors.json` contains six exact MessagePack binary vectors shared with the Java/Graal repo; the LLVM backend must match them byte-for-byte before parity is claimed. The first Java reference implementation is `dev.oreslang.runtime.MessagePackCollectionCodec`, **not yet integrated with ActorRuntime.send**.

**Native parity admission blockers:** The Java compiler currently erases `Array<T>` versus `List<T>` into `ListType(T)`, and all integer widths/unsigned or `bigint` aliases into `Primitive.INT`. LLVM must not reproduce these erasures in wire type identity: the compiler-generated native descriptor must preserve collection kind, numeric width/signedness and stable package-qualified schema identity. `schema_id` from untrusted input cannot select a host class or codec. Preserve a native bounded decoder state machine and require full byte-for-byte cross-runtime v2 vectors. Java's compiler-bound source actors now perform MessagePack encode/decode before mailbox admission; existing Java host capability transfers are separate from future serializable user messages, not evidence of wire equivalence.

The on-wire schema is not a memcpy of LLVM native structs, nor `java.io.Serializable`, nor a mutable JVM object frozen and passed by alias. Structs, tagged enum cases, maps, options, decimals, complex values, unsigned widths and opaque capability descriptors need additional compiler-generated codecs before admission on this ABI. Normalize string and number policies across all backends. For union/pattern domains use explicit stable variant or alternative tags when shape tests could overlap.

A later `SHARED` fast path may use `rt copy` **only** when the full value is statically Serializable and the copy is semantically equivalent to encode/decode, including no shared mutable aliases, bounds, validation, cancellation and failure behavior. Benchmark copy vs encode/decode at 8B, 64B, 1KB, 64KB and 1MB under scheduler contention; do not assume either choice wins. Source-visible `send` semantics stay identical across actor placement.

## Implementation and parity gates

1. Align a single cross-backend grammar: distinct `define pattern` (type only) and `define enum` (type plus variants), nullary and typed payload variants, `type T = A | B`, `match<R>` and `do match`, and `Actor<messages=T>` metadata. Add static tests for pattern includes enum, direct/indirect enum -> pattern rejection, and type-only names being nonconstructible. Ensure rejected syntax emits no IR. Do not conflate parser acceptance with feature support.
2. Introduce typed sum layout descriptors at the compiler/ABI boundary. Define nominal type identity, constant case tags, payload field ordering, alignments and ownership/drop semantics. Avoid a second actor registry or independent message executor.
3. Lower source constructors, tag comparisons, payload extraction, exhaustive pattern decision tables and arm result type checks into verified LLVM IR; compile, link and execute real Oreslang fixtures with Clang.
4. Integrate typed actor-message send/receive with the unified native ActorGroup, mailbox admission and CONTROL lane; test lifecycle, cancellation, panic and resource finalization with payloads.
5. Run a common positive and negative corpus against Java/Graal and LLVM pinned source heads, including duplicate labels, overlapping/unreachable patterns, nonexhaustive matches, mismatched payload types, unsafe cross-actor pointers, unknown serialized tags, schema version changes and PostgreSQL round trips.

**Milestone status:** the native compiler's current documented general-source subset remains restricted. The tests must show production source compilation and runtime behavior before marking this feature complete. Documentation alone is not implementation evidence.

**Executed native binary parity:** The header-only C++17
\`src/main/cpp/oreslang/runtime/MessagePackWireV2.hpp\` implements the bounded
v2 typed subset, using OpenSSL EVP SHA-256 for recursive wire schema hashing.
It has an optional CMake/CTest target and a Python differential test against
the exact shared JSON vector manifest.
[Funded native job #38169364693](https://github.com/gha-indie-worker/gha-indie-worker-docs/actions/runs/38169364693)
compiled the exact C++ source and passed six Java/Python golden vectors,
native round trips and malformed-input cases. This does not prove native
Oreslang compiler source-level actor send/receive integration.
