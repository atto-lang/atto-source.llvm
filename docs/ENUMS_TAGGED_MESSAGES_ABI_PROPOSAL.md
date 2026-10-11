# RFC: native enum and tagged message ABI parity

**Status: proposal only; no new LLVM source behavior or test results are claimed.**

Canonical design source (Java repo):
[ENUMS_TAGGED_MESSAGES_AND_WIRE_IDENTITY.md](https://github.com/ores-truffle-oreslang/oreslang-source.java/blob/design/enum-and-tagged-actor-messages-20261011/docs/proposals/ENUMS_TAGGED_MESSAGES_AND_WIRE_IDENTITY.md).

## Native requirements

Oreslang should distinguish nominal enums, optional typed-payload variants, and transparent structural unions. `match` is a value inspection expression/statement rather than a type declaration. A match expression may infer its result or constrain its result with `match<R>`; no per-pattern result annotation is needed. `do match` is statement-only. Integrate with the existing `over/on` proposal and do not unilaterally change match ordering/exclusivity during the enum work.

Proposed source fixture (NOT admitted by current LLVM source frontend):

```ores
define enum WorkerMessage as
  Start(job_id: string)
  Progress(done: int, total: int)
  Stop
end

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
```

## ABI constraints

- A backend-local discriminant is an implementation detail, not a portable identifier. Runtime pointer addresses, Java objects, ELF symbol names, LLVM enum ordinals, PostgreSQL OIDs and per-process interned Symbols must not enter the durable wire contract.
- Descriptor: nominal qualified type identity plus schema contract version; each case has a stable canonical string tag, typed payload layout and compiler-checked constructor arity and ownership.
- Native representation can optimize nullary cases as compact scalars and payload cases as discriminant + sized/aligned owned payload, provided constructor/destructor behavior, actor-heap lifecycle and FFI boundaries obey the same semantic contract as Java/GraalVM.
- No implicit casting from other enums with coincident labels. Reject missing/unknown tags on decode and avoid dynamic atom/Symbol interning from untrusted data.
- `Actor<messages=T>` and `send(T)` require both static and runtime validation; actor payload copying/moving may not create cross-actor writable aliases. Dispatch and receive occur on the actor's exclusive logical scheduler lane.
- Payload serialization must not rely on layout pointer bytes or padding; define an independent canonical codec, version, tagged structured payload, field names, numeric types and evolution policy before cross-process messages are enabled.
- Decoding a payload case for a stopped/restarted actor must honor generation fencing, mailbox cancellation and reclamation. No stale pointers into previous actor heaps.

## Implementation and parity gates

1. Align a single cross-backend grammar: `define enum`, nullary and typed payload variants, `type T = A | B`, `match<R>` and `do match`, and `Actor<messages=T>` metadata. Ensure rejected syntax emits no IR. Do not conflate parser acceptance with feature support.
2. Introduce typed sum layout descriptors at the compiler/ABI boundary. Define nominal type identity, constant case tags, payload field ordering, alignments and ownership/drop semantics. Avoid a second actor registry or independent message executor.
3. Lower source constructors, tag comparisons, payload extraction, exhaustive pattern decision tables and arm result type checks into verified LLVM IR; compile, link and execute real Oreslang fixtures with Clang.
4. Integrate typed actor-message send/receive with the unified native ActorGroup, mailbox admission and CONTROL lane; test lifecycle, cancellation, panic and resource finalization with payloads.
5. Run a common positive and negative corpus against Java/Graal and LLVM pinned source heads, including duplicate labels, overlapping/unreachable patterns, nonexhaustive matches, mismatched payload types, unsafe cross-actor pointers, unknown serialized tags, schema version changes and PostgreSQL round trips.

**Milestone status:** the native compiler's current documented general-source subset remains restricted. The tests must show production source compilation and runtime behavior before marking this feature complete. Documentation alone is not implementation evidence.
