# ActorGroup multi-membership and exclusive lifecycle ownership

Status: cross-backend behavioral contract. This is **not** a claim that the LLVM
source compiler exposes ActorGroup APIs yet.

## Two independent relationships

- A **cooperation membership** is `(runtime, actor incarnation, group)`. An
  actor may join several groups, with independent admission, event subscriptions,
  leave, close, and group-local delivery accounting. Joining a room must never
  implicitly adopt the actor for lifecycle management.
- A **lifecycle owner** is an exclusive, single authority for stop/cancel,
  teardown and completion. A second ActorGroup must never acquire independent
  management rights over a live actor. In Java `managedByGroup` enforces that
  exclusivity; in LLVM `NativeActorGroup::adopt` uses the process-local
  exclusive-owner registry. Future source supervisors must reconcile this with
  the single supervisor/structured-child owner instead of introducing a second
  competing stop authority.
- Neither cooperation membership nor possession of a group handle is permission
  to read another actor's private memory, mutate its WebSocket, or cancel it.
  Messages cross actor boundaries through typed `ActorRef`/mailbox transport.

## Example: one WebSocket session in two chatrooms

A long-lived **connection actor** exclusively owns the WebSocket and processes
outgoing writes on one serialized lane. The room groups are independent
cooperation domains. The connection actor can be a member of rooms A and B;
alternatively room managers can hold authorized references to the connection
actor without requiring it to subscribe to a group bus.

1. An authenticated join request is authorized for a particular user and room.
   Issue a scoped, runtime-affine join capability or have the room manager
   register an authorized member. Joining one room cannot authorize another.
2. Client input is parsed once by the connection actor, routed to the selected
   room's actor/mailbox, and checked against live room membership before
   broadcast. Do not trust a room ID contained in a client frame by itself.
3. Each room fans out via bounded actor mailboxes (safe for private/isolated
   connection actors), or via a dedicated `SHARED` subscriber forwarding
   sanitized typed messages to a private connection actor.
4. Outbound traffic from both rooms converges on the connection actor's one
   mailbox/writer; no room writes to its socket concurrently. Use per-room
   sequence IDs when ordering/replay matters, because different room streams
   have no combined total order.
5. Leaving room A atomically revokes A's membership and closes only A's event
   subscriptions. Room B and the WebSocket remain alive. Closing a room stops
   only its explicitly managed children, not membership-only connections.
6. Disconnection terminates the connection actor; the runtime removes all of
   its group memberships and subscriptions. A new connection uses a fresh
   actor incarnation and must independently reauthorize its room joins.

Important existing Java restriction: `ActorEventBus.subscribe` admits
`SHARED` actors only. `PRIVATE` and `UNTRUSTED` actors can join a group but
must use mailboxes for room delivery until isolation-safe event subscriptions
exist. A WebSocket-owning actor should not become `SHARED` merely to opt
into zero-copy group events.

## Hardening invariants and conformance gates

- Joining A and B never changes actor identity, actor mailbox seriality, or
  lifecycle owner; duplicate joins of the same group are idempotent.
- Close/leave A revokes A event access and drains A subscriptions/pending
  reservations without affecting B; A's stale join capability is rejected.
- Group manager cancellation affects explicitly managed children only. A group
  that merely knows an ActorRef or admits a member cannot stop that member.
- Independent room membership limits, per-connection room limits, bounded topic
  subscriptions, mailbox quotas and outbound byte budgets prevent fanout abuse.
  Excess load is rejected, backpressured, or disconnects a slow client by
  documented policy, never accumulated unboundedly.
- Use `(runtime, actor ID, generation)` for memberships/owner lookups in native
  and distributed implementations to prevent actor ID reuse/ABA. Joining,
  leave, actor death and group close must be linearizable and leak-free.
- Authorization checks are required on room send, read/subscription and
  membership mutation, not just at initial WebSocket handshake. Revoked users
  cannot regain access with a cached room ID or obsolete capability.
- A successful group publish or mailbox admission is not a recipient ack, a
  durable delivery guarantee, or proof that bytes reached the network.
  Connection failures must be observed through the connection actor lifecycle.
- Differential tests must include: two-room join; leave/close A with B alive;
  stop/close the connection owner; racing leave/publication; stale references;
  cancel/restart; slow client; cross-runtime join rejection; room authorization
  revocation; actor generation reuse; zero-copy isolation rejection.

## Current backend state

Java/GraalVM host runtime: `ActorRuntime.ActorGroup` now maintains a bounded,
reverse-indexed, per-actor set of cooperating groups. Admission is serialized
across rooms; duplicate joins are idempotent and leave, group close and actor
retirement reclaim slots. `managedWorkers` remains a separate, exclusive
lifecycle registry. `ActorGroupMultiMembershipTest` covers two-room
participation, private zero-copy restrictions, room leave/closure and quota
reclamation. This is a host runtime API, not yet full source-language lowering.

LLVM: `NativeActorMembershipRegistry.hpp` now implements a distinct,
per-runtime, bounded *host-side* cooperation registry with revocable join
permits, incarnation fencing, atomic multi-room admission, leave, group close
and actor retirement. `NativeActorGroupLifecycle.hpp` still enforces one
independent owner for stop/cancel/finalization. The new native regression
compiles with strict warnings and passes ASan/UBSan in the private exact-source
test mirror. These APIs are not yet source-level `ActorGroup` lowering,
actor message authorization, room persistence or isolation-safe event
subscriptions. Keep unsupported guest constructs fail-closed.

Production WebSocket service routing remains an application-layer feature:
only the connection actor owns socket writes, an authenticated room actor
validates every message, and its per-client outbound mailbox must have a
bounded byte/message budget with an explicit slow-client policy. The host
membership registry must not be used as a substitute for application ACLs.
