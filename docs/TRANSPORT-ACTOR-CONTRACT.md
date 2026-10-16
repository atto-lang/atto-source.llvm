# Proposed typed network-actor language contract

Status: **proposal for independent Java/Graal and C++/LLVM compiler teams**.
Nothing in this repository is executable or currently mandatory. Companion
runtime hardening: [Graal HTTP request lifetimes PR #529](https://github.com/ores-truffle-oreslang/oreslang-source.java/pull/529).

## Two-axis network-actor concurrency contract

User-exposed concurrency configuration is **protocol-specific**:

| Declaration | User-configurable concurrency dimensions |
| --- | --- |
| `actor` / `isoactor` | `messages` only |
| `http actor` | `connections`, `messages_per_connection` |
| `tcp actor` | `connections`, `messages_per_connection` |
| `ws actor` | `connections`, `messages_per_connection` |

`messages=N` controls how many ordinary actor receive handlers may be
**in flight** at once. Network actors have **exactly two** user-facing
concurrency values: distinct transport sessions assigned to the actor,
and concurrent in-flight message handlers admitted **for each such
session**. These are admission/continuation limits, not the number of
threads simultaneously executing guest code. An `await` relinquishes
the carrier but retains the handler's message permit until completion,
failure or cancellation. The actor-wide one-writer execution lease is
**fixed**, not a third user-configurable concurrency knob.

A connection is an opaque, incarnation-fenced broker session, **not**
an HTTP request, JVM exchange or transferable raw OS FD. In HTTP/1.1
this corresponds to a keep-alive connection; HTTP/2 and HTTP/3 may
multiplex many concurrent requests/streams over one connection.
`messages_per_connection` counts admitted application-delivered
`receive_http`/`receive_tcp`/`receive_ws` envelopes (one
handler invocation per envelope), **not bytes, TCP segments, WebSocket
fragments, or total lifetime message count**. A connection with a
completed or suspended handler can receive subsequent envelopes,
subject to its active-message limit. When the request itself spans
several envelopes, retain the request ID and generation in each.

An actor may therefore retain up to `connections *
messages_per_connection` in-flight network handlers if host-wide
resource budgets permit. Message work is fair-scheduled between
connections; all guest entrypoints and continuations continue to
acquire the same **one-writer actor execution lease**. Bounded queues
outside the admitted set, transport backpressure, and process/tenant
limits remain separate host security/resource controls, not additional
actor declaration concurrency knobs. Saturated per-connection capacity
must not stall unrelated sessions. Client disconnect cancels that
session's admitted message handlers only; actor shutdown revokes all.

**Proposed surface only (not parser-accepted):**

```ores
@concurrency(messages = 16)
define actor BackgroundWorker as
  receive(ActorMail<String> m): void { }
end

@concurrency(connections = 128, messages_per_connection = 8)
define http actor WebHandler as
  receive_http(HttpMessage m): void { }
end
```

The proposal has a strict [HIR concurrency envelope schema](../contracts/actor-concurrency.v0.schema.json):
`generic` carries **only** `messages`; `http`, `tcp`, and `ws`
carry **only** `connections` and `messages_per_connection`. Additional
concurrency settings are rejected, not silently ignored.

The annotation spelling, bounds, defaults, dynamic reconfiguration
protocol, and diagnostic codes are not implemented yet. Admission
is to be enforced by the runtime transport broker, not trusted to
application actors.

## Construction and per-instance concurrency overrides (proposed)

An actor is a specialized class **declaration**, not a caller-owned
class instance. Ordinary `new Class(args)` still runs a class
constructor immediately. `new Actor(args)` must instead produce an
**inert** `ActorSpec<Actor>`, carrying validated actor constructor
arguments and optional per-instance concurrency overrides. It must
not run actor code or allocate actor state in the caller. `spawn spec`
atomically consumes the spec, reserves capacity and creates an
`ActorRef<Actor>`. Actor field initialization, a future explicit
`constructor(...)`, and `on_start()` run only within the child actor
domain, in that order. Constructor execution is synchronous; async
initialization belongs in `on_start()`.

```ores
// All forms here are proposed, except existing 'spawn Worker()'.
val regularSpec = new Worker(42); // ActorSpec<Worker>, no running actor.
regularSpec.set_concurrency(6);   // ONE control: messages.
val regularRef = spawn regularSpec;

val httpSpec = new WebHandler();
httpSpec.set_concurrency(128, 8); // TWO: connections, messages per connection.
val httpRef = spawn httpSpec;

val defaultRef = spawn Worker();   // Existing source syntax: preserve it.
```

Java/Graal PR #529 now parses `spawn Worker()`,
`spawn new Worker()`, and `spawn (new Worker())` as the **same AST**,
with unchanged actor ownership checks and no constructor arguments.
The zero-argument alias is implemented, but standalone `new Actor()`
spec values and actor constructors remain future work.

`spawn Worker(args)` can desugar to `spawn new Worker(args)`. Parsing
`spawn spec` must distinguish a typed spec expression from a
type-name constructor invocation, including qualified imports.

`ActorSpec<T>` has only built-in, non-overridable configuration methods.
For `generic`/`isoactor`, `set_concurrency(messages)` has arity 1;
for `http`/`tcp`/`ws`, `set_concurrency(connections,
messages_per_connection)` has arity 2. The fixed one-writer actor
execution lease is not configurable. A declaration
`@concurrency(...)` specifies defaults; pre-spawn `set_concurrency`
overrides them for **one** spawned actor. Effective values are validated
against supervisor/tenant budgets and actual backend capability at
spawn; no silent clamping or false promise that suspended receive
interleaving is implemented.

Oreslang `const` forbids referent mutation; `val` is the correct
binding for `spec.set_concurrency(...)`. Alternatively, a functional, **consuming**
`with_concurrency(...)` should yield a new spec for readonly
`const spec = new Worker().with_concurrency(6)`. Its original affine
spec is consumed, not copied or left with a duplicate spawn permit.
A spec must not be copied across mailboxes or reused after consumption. A running
`ActorRef<T>` **cannot** expose synchronous `set_concurrency`;
future live adjustment, if needed, is a separate supervisor-authorized,
asynchronous and versioned reconfiguration operation.

For Java/Graal implementation details and full validity/acceptance
matrix, see [actor spawn specification](https://github.com/ores-truffle-oreslang/oreslang-source.java/blob/harden/http-request-scoped-actor-handoff-20261009/docs/ACTOR_SPAWN_SPEC_DESIGN.md).
**None of these actor-constructor/spec/builder forms have parser or
runtime support yet.**

## Source declaration shape

```ores
define http actor Handler as
  receive_http(HttpMessage m): void {
    val req = m.get_request();
    val res = m.get_response();
    await res.start(200, "text/plain");
    await res.write("first");
    await res.write("second");
    await res.finish();
  }
end

define tcp actor TcpWorker as
  receive_tcp(TcpMessage m): void {
    val conn = m.get_tcp_conn();
    if m.kind == TcpEvent.data; do
      await conn.write(m.bytes);
    fi
  }
end

define ws actor WsWorker as
  receive_ws(WsMessage m): void {
    val conn = m.get_ws_conn();
    if m.kind == WsEvent.text; do
      await conn.send_text(m.text);
    fi
  }
end
```

These spellings are **not yet supported by either compiler**.
They denote an actor's *ingress protocol*, not its isolation/trust level.
`actor` vs `isoactor`/policy, trusted vs untrusted execution,
and AOT vs JIT remain orthogonal. Standard
`receive(ActorMail<T>)` has no implicit request/connection capability.

## Normative semantics to implement before admission

1. `actor_execution_lease(actor_id)` is singular. Every ingress,
   continuation, timer, on-start, supervision event and cleanup routine
   enters through the mailbox-backed scheduler. No two worker/carrier
   threads execute guest instructions for the same actor simultaneously.
2. Ordinary actors configure one `messages` in-flight handler
   limit; transport actors configure independent `connections` and
   `messages_per_connection` handler limits. Each permit lives across
   suspension and is released on completion/cancel, not per I/O call.
   The actor-wide single guest-execution lease is invariant, not an
   additional user control. Fair interleaving of suspended handlers
   needs an actor-lease-safe scheduler; it is not yet established by
   the existing source actor implementation.
3. HTTP envelope identity is `(request_id, request_generation,
   actor_incarnation, event_sequence)`, not just a connection FD.
   TCP/WS envelope identity uses `(connection_id, generation,
   actor_incarnation, event_sequence)`. IDs cannot be reused inside the
   receiver's accepted incarnation; stale events are rejected.
4. Every `HttpMessage` refers to exactly one request.
   `get_request` exposes a bounded read-only request view;
   `get_response` exposes only that request's revocable response sink.
   Multiple messages may refer to the same request; they must never
   alias any *other* response sink. No global `current_request`/
   `current_response` field exists.
5. Response sinks have a terminal state and exactly one writer. Valid
   operations are `start(status,headers)`, ordered bounded
   `write(bytes)`, `finish()` or `abort(reason)`; all network I/O
   operations return futures that yield actors off their carriers.
   Header commit happens once. Backpressure and deadlines are enforced
   at the transport, not entrusted to guest code.
6. Each request has its own deadline, cancellation and completion.
   Killing request A must not kill long-lived actor B's other requests.
   An actor spawned solely for one request may have a dedicated
   actor-finalization barrier. Actor termination revokes all open sinks;
   already completed requests must not remain retained in an actor's
   lifetime callback list.
7. TCP messages are typed connection events: `open`, `data`,
   `peer_half_closed`, `closed`, `error`. Never dispatch user
   code through `conn.on("message", callback)` outside the actor
   ingress scheduler.
8. WebSocket messages are typed session events `open`, `text`,
   `binary`, `ping`, `pong`, `close`, `error`.
   `WsMessage` is not `TcpMessage`: framing, fragmentation, masking
   and close handshakes belong to the transport. Send commands are
   `send_text`, `send_binary`, `ping`, `close`.
9. A client connection FD is **not** an HTTP request. Keep-alive,
   HTTP/2 streams, TLS and HTTP/3/QUIC require broker-owned
   transport/protocol state. Do not move the client FD per HTTP request.
   If a trusted native actor explicitly receives an exclusive socket,
   require a quiescent connection and separate explicit OS-level transfer.
10. For untrusted actors, the broker copies/serializes bounded request
    metadata, body chunks, and transport events. It accepts validated
    response commands, verifies actor incarnation, request generation
    and event ordering, enforces time/size/backpressure limits, and never
    exposes pointers, Java objects, raw FDs, TLS keys or host callbacks.
    Serialization alone does not sandbox malicious guest code.
11. No implicit authority on deserialization. A response command may be
    accepted only if the broker independently proves current ownership
    and stream state. Reject duplicated/out-of-order terminal commands,
    wrong actors, revoked epochs, invalid headers, oversized buffers and
    unauthorized protocol upgrades.
12. The actor's one-guest-segment execution lease includes **all**
    async continuations, irrespective of in-flight handler counts.
    `messages` or `messages_per_connection` greater than one permits
    logically overlapping handlers whose suspended work may interleave,
    **not simultaneous guest execution on multiple carrier threads**.
    No implicit transport callbacks can bypass this invariant.
13. The broker must prove a connection's admission and sequence against
    the correct actor incarnation, enforce both network quotas
    independently, release a message slot exactly once on all terminal
    paths, and reclaim an entire connection's slots upon close. A failed
    quota admission must not consume its sequence/ID. Test queue limits
    and slow-consumer behavior without inventing a third public
    concurrency parameter.

## Future HIR / MIR contract

A future HIR `ActorDecl` should preserve:
`ingress_protocol = generic | http | tcp | ws`,
`entry_method`, `envelope_type`, `actor_kind`,
`trust_policy`, source span, declared capabilities, compilation mode,
and **either** `concurrency.messages` (regular) **or** the pair
`concurrency.connections` / `concurrency.messages_per_connection`
(network), never both forms.

HIR additionally needs `ActorConstructorDecl` with parameter
signature, actor-only initialization effect and visibility policy,
plus `ActorSpecCreate`, `ActorSpecConfigure`, and
`ActorSpecConsume` nodes carrying the concrete actor type, frozen
constructor-argument schema, declaration defaults, instance override
and source-span diagnostics. An `ActorSpec` is affine/non-sendable,
distinct from both `ActorType` and `ActorRef<ActorType>`.
No actor instance initializer or constructor can execute during
`ActorSpecCreate`.

MIR must express `AcquireActorLease` / `ReleaseActorLease`,
`IngressDequeue`, `SuspendIo`, `ResumeContinuation`,
`CheckExchangeGeneration`, `RespondStart/Chunk/Finish/Abort`,
and `RevokeExchange` as *conceptual operations*, **not** defined
instruction opcodes yet. A backend must prove these semantics without
sharing a heap or an OS descriptor representation.

## Minimum shared conformance gates

- Constructor and spawn conformance: `new Worker()` must be inert,
  `spawn spec` consumes once, and actor constructor/`on_start` run in
  actor domain before `ready`. Compiler rejects wrong overload arity,
  mutating a `const` spec, post-spawn mutation, double spawn,
  constructor arguments escaping isolation, and `ActorRef` methods on
  unspawned specs. Bound checks reject quota escalation.
- Parser accepts correctly qualified declarations, rejects duplicate/
  conflicting `receive*`, mismatched message types, ordinary actor
  methods masquerading as transport ingress and invalid callback escapes.
- A regular actor admits at most N logically active handlers. Network actors
  independently enforce C live connections and M in-flight handler envelopes
  on **each** connection; rejected admission consumes no sequence, and
  completed/cancelled handlers free permits without freeing a live connection.
  Duplicate connection IDs and stale actor incarnations fail closed.
- Two simultaneously admitted HTTP requests on one actor produce distinct
  responses, IDs and completion events. Two messages per same request
  retain one correlation ID; A/B/A event interleaving never swaps sinks.
- Wrong-owner, duplicate-claim, stale generation, actor reincarnation,
  failed dispatch and response-after-terminal are rejected.
- Request A timeout/abort leaves request B alive; actor termination
  closes all outstanding requests and does not retain completed ones.
- Slow-read/write adversarial tests validate buffers, deadlines and no
  blocking carrier; TLS/HTTP2 sessions cannot leak other streams.
- TCP half-close and WS ping/pong/fragmentation/close are serialized
  as scheduler messages; no guest callback runs on an I/O thread.
- Untrusted worker tests verify copied payload immutability, malformed
  frame rejection, bounded command queues, forced crash cleanup and
  complete absence of raw memory/FD capabilities.

The teams must keep frontend and backend tests independent until a
common HIR/MIR implementation actually exists. Do not advertise the
syntax as implemented because a contract document was merged.
