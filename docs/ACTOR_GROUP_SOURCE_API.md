# Source-level ActorGroup contract (staged Java / LLVM parity)

The source language has **two orthogonal concepts**:
- `ActorGroup`: a cooperative membership domain; NOT a scheduler, supervisor, or owner.
- `ActorGroupJoinCapability`: an opaque, runtime-affine admission authority. Its
  serialized form is deliberately unavailable.

An actor can belong to multiple groups. Only its single supervisor/structured
lifecycle authority may cancel it. Group membership alone never grants access
to an actor's memory, other mailboxes or I/O resources.

## Canonical source surface

The following source expressions are the initial Java/GraalVM lowering target:

```ores
val group = ActorGroup.new();
val permit = group.join_capability(); // group manager only
// Inside a running actor turn:
permit.join();                         // idempotent current-actor join
group.member_count();                  // member only, or trusted manager
group.leave();                         // revoke only current actor
group.is_closed();                     // no privilege escalation
group.close();                         // creator/supervisor only
```

These are **direct-call-only** operations and accept no generic type or
positional arguments. The language types `ActorGroup` and
`ActorGroupJoinCapability` are opaque, non-data-only capabilities. They are
NOT automatically serializable, `Copy`, or safe to put in an actor mailbox.
In particular, no arbitrary `ActorRef.send(permit)` exception is allowed just
because the runtime has an internal permit object. A secure explicit
capability-transfer protocol must separately prove provenance, receiver
scope, generation, isolation and revocation before that can be enabled.

A group creator is automatically a cooperating member and remains the creator
after `leave()`. Closing a group cancels explicitly managed workers only;
ordinary cooperating members continue executing under their own lifecycle
owners. A creator's termination closes every group it created, including those
the creator voluntarily left. Repeated joins are idempotent and count against
membership quota once.

Join caps are manager-issued and rotate/revoke on close. Joining requires a
live actor context; a trusted host `main` may create and close groups but
cannot call `permit.join()` for itself. An actor may not join a different
actor by invoking these source methods on its behalf. Untrusted actor code
does not gain ambient group-creation rights. Java interop/reflection must
never be required or implicitly enabled.

## Security audit hardening

The Java compiler must reject `ActorGroup` and
`ActorGroupJoinCapability` at every actor mailbox/output boundary, not
just ordinary shared-memory validation. Nested containers cannot launder
a capability into sendable data. Source and runtime admission must both
enforce creator `ACTOR_SPAWN` authority, including calls through helpers.

For the native host registry, the join-permit origin identity is
strictly **nonwrapping**: new instances fail closed at counter exhaustion,
so obsolete permits cannot regain authority after an integer overflow.
Membership remains separate from exclusive lifecycle ownership, and the
LLVM guest source language still rejects ActorGroup constructs until
typed lowering and supervisor authority are implemented.

## Delivery and authorization

This slice exposes **membership** only. A production room message needs a
typed mailbox/room actor; the receiving room must check live membership and
the application ACL at admission, not just at WebSocket handshake. The
`SHARED` event bus is not exposed to private actors merely for convenience.
Only the connection actor owns socket writes. Successful send/publish
admission is not network delivery or a durable acknowledgment.

## Backend gates

**Java/GraalVM**: TypeChecker and OresEvalRootNode supply the initial direct
source operations and runtime capability dispatch. Dedicated tests cover
typed admission, callable arity, forbidden capability mailbox payloads,
manager-only operation, and isolated actor multi-group use. This does not yet
implement secure capability transport between independent actors or
supervisor declarations in source.

**LLVM**: NativeActorMembershipRegistry provides bounded, generation-fenced
host control-plane primitives. LLVM guest-source `ActorGroup.new`, group
methods, and capability joins remain **unsupported** until typed MIR lowering,
supervisor identity and nonblocking continuations can enforce the contract.
Native source conformance fixtures must reject those forms with nonzero
status and no emitted LLVM IR. Do not treat host-only C++ tests as source
parity.

## Promotion gates

1. Run Java source-level execution/rejection suite against exact changed
   source, including concurrent actor life/death and revocation.
2. Implement a purpose-built typed capability-transfer envelope for
   supervisor-authorized cross-actor joins; no ambient host-object escape.
3. Lower LLVM source membership handles and authoritative admission, with
   generation fencing and fail-closed ownership.
4. Test `await`/cancellation/room fanout using pinned continuation frames;
   never block an OS carrier.
5. Add a two-room WebSocket acceptance fixture with bounded outbox, slow-client
   policy, disconnect cleanup and Java/LLVM differential verification.
