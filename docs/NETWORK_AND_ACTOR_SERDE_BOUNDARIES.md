# Two serialization contracts: network vs supervisor/actor DATA

Status: **shared-language contract and native admission audit**, 2026-10-15.
The canonical Oreslang declarations are in
`oreslang-serialization-and-validation/src/schema/transport_interfaces.ores`:

- `NetworkCodec<T>`: encode/decode untrusted bytes for HTTP/TCP/WS/RPC.
- `ActorMessageCodec<T>`: encode/decode typed, immutable actor **DATA**
  messages, including supervisor→actor and actor→actor.

There is **no** automatic `Serializable` bridge between them. Encoding
primitives may be shared, but no untrusted network bytes may select a
supervisor/actor codec or mint an actor capability. Supervisor CONTROL
(`on_panic`, `on_raise`, `on_throw`, teardown) remains a third, sealed
runtime-only lane and does NOT expose a user-implementable source interface.

## Current native implementation boundary

- `MessagePackWireV2.hpp` implements bounded native actor wire v2
  components: schema ID, type-shape digest and canonical payload shape.
  Its digests provide **shape commitment**, not sender authentication.
- The LLVM source compiler currently supports only a small i64 actor-send
  payload subset. It does not lower generic `NetworkCodec<T>` or
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
