# Two serialization contracts: network vs supervisor/actor DATA

Status: **shared-language contract and native admission audit**, 2026-10-15.
The canonical Oreslang declarations are in
`oreslang-serialization-and-validation/src/schema/transport_interfaces.ores`:

- `NetworkCodec<T, F>`: encode/decode untrusted bytes for HTTP/TCP/WS/RPC.
- `ActorMessageCodec<T>`: encode/decode typed, immutable actor **DATA**
  messages, including supervisor→actor and actor→actor.

`F` identifies an explicitly selected JSON/MessagePack/Protobuf external
wire profile; the native LLVM compiler does not yet enforce or lower this
generic. `Sendable<T>` is a compiler-owned transitive ownership proof, not a
user-implementable marker. There is **no** automatic `Serializable` bridge between them. Encoding
primitives may be shared, but no untrusted network bytes may select a
supervisor/actor codec or mint an actor capability. Supervisor CONTROL
(`on_panic`, `on_raise`, `on_throw`, teardown) remains a third, sealed
runtime-only lane and does NOT expose a user-implementable source interface.

## Current native implementation boundary

- `MessagePackWireV2.hpp` implements bounded native actor wire v2
  components: schema ID, type-shape digest and canonical payload shape.
  Its digests provide **shape commitment**, not sender authentication.
- The LLVM source compiler currently supports only a small i64 actor-send
  payload subset. It does not lower generic `NetworkCodec<T, F>` or
  `ActorMessageCodec<T>` implementations or implement a general
  source-level `Serializable` registry.
- Source-level `Pointer<T>` remains fail closed (see pointer PR #416).
  Exposing native graph slot/generation handles in wire bytes is prohibited,
  even if those handles appear to be scalar integers.
- Native `NativeSupervisorRegistry`, `SupervisorControlTurn`, and similar
  CONTROL substrates must retain authenticated supervisor-issued identity,
  generation and execution-lane checks. DATA deserialization is never
  an authorization mechanism for CONTROL.

## Borrow and ownership rules at native boundaries

1. Encode from a synchronous immutable borrow **only** while the owner is
   live. Do not persist `borrow T` in MessagePack output, a channel, a
   decoder, a callback, an FFI proxy or a suspended continuation.
2. Decode to independently **owned** values in the target ownership domain.
   If the native decoder later offers borrowed input slices, their lifetime
   is tied to a pinned immutable input-buffer lease; such slices cannot
   outlive the buffer, cross `await`, or become actor messages.
3. Actor send must serialize/validate to staged owned bytes, reserve queue
   capacity and verify recipient epoch, then atomically commit movement of
   the sender-owned payload. On encoding, schema, quota, epoch, enqueue or
   cancellation failure, the sender must still own its value and any staged
   bytes must be retired **exactly once**.
4. For fixed-layout zero-copy transfer, use a separately proven transferable
   owned buffer with same-domain/pin/deregistration guarantees and consumer
   completion ACK. Merely returning a native pointer or `rt share` is not
   an allocation-free or isolation-safe operation.
5. Both contracts recursively reject raw/managed Pointer<T>, borrows,
   channels, observables/streams, futures, mutexes, closures, arbitrary host
   objects, actor capabilities and any nested equivalents. A stable,
   explicitly schema-authorized actor *data ID* is not execution authority.
6. Deserialization must enforce canonical encodings, fixed depth/bytes/node
   limits, version, schema, exact numeric width and enum tags; malformed
   values must fail before memory allocation or partial actor admission.

## Required parity tests before enabling generic codecs

- Java+LLVM parse the same interface declarations, preserve their
  independent symbols in type metadata, and reject codec capability values
  as actor message payloads, including nested Option/Result/Array/List.
- Network parser rejects actor DATA envelope confusion, forged schema IDs,
  malformed UTF-8, trailing/duplicate data, incomplete frames, overlarge
  lengths, and stale message generations. Neither codec accepts CONTROL.
- Host/native send tests cover borrow-vs-move, mutation during encode,
  send admission rollback, cancellation, actor stop/restart, ABA, and
  producer/consumer lease quiescence under ASan/TSan.
- Explicit proof of no shallow mutable alias across actor boundaries;
  independently measure byte copies and allocations. No production
  zero-copy claim based solely on MessagePack component parity.
- Execute exact-head Maven/native-image, native compiler build/CTest,
  cross-runtime golden vectors and real compiled actor-to-supervisor and
  supervisor-to-actor round trips before enabling source codecs.

Related: `docs/audits/MESSAGEPACK_ACTOR_TRANSPORT_AUDIT_20261011.md`,
`docs/NATIVE_ACTOR_SEND_OWNERSHIP.md`, `docs/NATIVE_ACTOR_SEND_OWNERSHIP.md`
and `docs/SUPERVISOR_SCHEDULER_MAILMAN_PARITY.md`.


## Erased callable type and native backend parity gate

`AnyCallable` is the sealed compiler-native erased supertype of reifiable
function values (the callable subtype hierarchy is `Fnc<signature> <:
AnyCallable <: Any`). It is **not** a user-implementable interface or
trait; defining a `call` method does not make an object callable.
`AnyCallable` does not carry parameter/return metadata that would permit
direct invocation, and a concrete signature must be statically proven
before a call. Non-reifiable `routine` declarations do not become ordinary
callable values.

Crucially, neither `AnyCallable` nor a closure/function value or a
record/list/tuple containing one can be `Sendable`, `Serializable`,
a generic `NetworkCodec<T,F>` payload, or an `ActorMessageCodec<T>`
payload merely by virtue of being locally callable. Such values may hold
captured references, borrows, host handles, and actor CONTROL authority.
Never serialize their environment or reconstitute executable pointers
from untrusted bytes. Protobuf/JSON/MessagePack are **data codecs**, not
function-pointer, privilege or callback-ABI codecs.

The native source compiler **must reject** a program that requires
`AnyCallable` ABI/subtyping/dynamic narrowing, until native callable value
representation and ownership semantics have been explicitly implemented
and tested. Parsing its spelling as an opaque nominal and emitting an
`i64`/pointer or treating it as a network/actor DATA value is not parity.
This fail-closed policy also applies to mixed Java/native interface schemas
and to module-qualified builtin lookalikes.

Native parity cases to add before support is claimed: ordinary concrete
function -> `AnyCallable` widening; rejection of `AnyCallable()`,
`AnyCallable.user_property`, ordinary `class call()` impostors and
`routine`-as-value; rejection of nested callable/codec capability actor
payloads; capture and construction-`self` lifetime enforcement; pinned
schema and receiver epoch checks under malformed/hostile wire inputs.
None of these native source tests is implied by the existing MessagePack
component tests.
