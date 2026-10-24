# Observable, Stream and Channel — cross-backend contract v0 (2026-10-11)

**Status: target contract, not an implemented cross-backend ABI.** Keep the
document byte-identical in `oreslang-source.java` and `oreslang-source.llvm`.
Do not promote this version to a supported feature until executable guest
conformance and real transport integration pass on both backends.

## Separation of responsibilities

| Primitive | Meaning | Ownership | Backpressure |
|---|---|---|---|
| `Channel<T>` | Typed point-to-point transfer, optionally buffered | Domain-local sender/receiver capabilities | Bounded slots; zero capacity is rendezvous |
| `ReadableStream<T>` | One logical asynchronous sequence with an exclusive reader | Reader lease owned by actor/execution context | Consumer demand; at most bounded in-flight chunks/bytes |
| `WritableStream<T>` | Asynchronous sink | Single writer or explicitly shared synchronized sink | Awaited admission and flush barrier are different |
| `DuplexStream<TIn,TOut>` | Paired read/write directions with separate half-close | Owning connection/session actor | Both directions independently bounded |
| `Observable<T>` | Subscription factory for asynchronous event observation | Each subscription owns its cursor and quota | Explicit RELIABLE / LATEST / LOSSY policy; never implicit unbounded multicast |
| `Subscription<T>` | Cancellation-capable reader of one observable | Non-sendable actor-local capability | Demand/credit accounting per subscription |

The language/runtime owns these abstractions. Java `InputStream`,
`Flow.Publisher`, host `CompletableFuture`, raw file descriptors, C++ pointers,
and socket handles must not appear in a guest type, actor envelope or ABI.
Host adapters translate to typed Oreslang values with explicit capabilities.
Generic channels and subscriptions are **not sendable to other actors**; only
typed data envelopes pass through `ActorRef` mailbox transport.

`Observable.from_channel(channel)` creates a **single-consumer** subscription
unless an explicit broadcast policy is requested. Creating multiple independent
readers must never silently duplicate/steal a channel's messages.
`channel.stream()` is a single reader of the same underlying channel;
releasing an exclusive reader must preserve the cursor without racing a pending
read. `Observable<T>` multicast requires per-subscriber bounded credits,
delivery policy and independent cancellation. A channel/stream conversion may
not invent unbounded buffering or a background guest thread.

## Canonical reactive terminal ABI

A backend must not infer terminal meaning from `Option<T>` alone.

The lossless **consumer signal** is:

`Result<Option<T>, Option<E>>`

- `Ok(Some(value))` = NEXT(value)
- `Ok(None)` = COMPLETE
- `Err(None)` = CANCELLED
- `Err(Some(error))` = ERROR(error)

This nesting deliberately preserves `T = Option<U>` and
`T = Result<A,B>` as ordinary NEXT data. An optional error payload is also
unambiguous: ERROR(None) is `Err(Some(None))`, distinct from CANCELLED.

A producer data channel normally carries the three-way frame
`Result<Option<T>, E>` (NEXT / COMPLETE / explicit ERROR). Consumer
cancellation is local control and must not be serialized into the producer
frame unless a higher-level wire protocol explicitly owns such a cancellation
message.

A backend without source `Channel<T>`, Future selection, or the closed
Option/Result lowering required by this ABI must reject the reactive source
before lowering. Native mailbox/Future helpers and syntax-only negative tests
do not count as implementation.

## Mailboxes and serial actors

The actor mailbox transports typed `Envelope<Message>` using a private
`Channel` substrate. Envelope admission also validates incarnation,
sender capability, payload ownership/freeze, byte/message budget, sequence,
cancellation, and supervisor state. The **mailbox layer**, not ordinary
`Channel<T>`, owns logical handler-processing credit.

- Ordinary `Channel<T>(capacity=0)`: a writer completes admission once a
  reader commits the rendezvous; this does **not** mean the reader has
  finished processing.
- **Serial actor**: `max_active_lifetimes=1`, `queued_user_messages=0`,
  and a **single gate shared by HTTP requests and actor messages**. A
  one-credit delivery reservation is held from handler admission through
  `await`/suspension, `defer`, `finally`, `recover`, state-loan release,
  stream/response finalization and post-handler acknowledgement.
- The implementation may use an unbuffered channel plus a separate lifetime
  reservation, or a capacity-one acknowledged port. These are implementation
  alternatives; **a raw zero-capacity rendezvous channel alone is incorrect**.
- Public `send` is an admission attempt, not a processing acknowledgement.
  Future `send_async` must suspend the producer without parking an OS
  carrier and without retaining an unbounded waiter. `send_and_wait` / request
  reply requires explicit reply or completion protocol.
- An unavailable serial receiver must not make guest `send` block an OS
  thread. `try_send` fails immediately; awaitable admission uses bounded
  sender waiters, fair arbitration, cancellation and deadlines. Sending to
  the same serial actor while holding its lease cannot suspend awaiting
  itself: fail with a structured reentrancy/deadlock diagnostic.
- Internal runtime continuation/control envelopes use separately bounded,
  reserved headroom and never enter the user message channel. Only the owning
  actor's serialized lane may resume guest continuations.
- `receive()` for the next user message is callable only once the prior
  user handler has *logically* completed. Suspended handlers retain the lease
  even when a physical carrier was returned to its pool.
- Cancellation/stop seals admission and atomically revokes or settles waiting
  requests. Stale incarnation/generation tickets, double completion and
  post-stop resume must be rejected. Physical state deallocation requires
  quiescence of active carriers, loans and transport I/O.

### Awaitable actor-mailbox admission linearization

Awaitable actor send is a mailbox operation layered over bounded Channel
mechanics, not a retry loop and not another unbounded queue.

- `send(message)` remains an immediate admission attempt. A full USER lane
  fails with an explicit mailbox-full result; it never parks a carrier.
- `send_async(message)` / selectable outbox admission may suspend the logical
  producer, but the runtime must retain only a finite number of producer
  admissions. The bound covers the whole pre-publication cohort — payload
  preparation/freeze, registered waiters, transferred credits and committing
  envelopes — not merely entries already visible in a waiter deque. This bound
  is independent of queued message count, bytes, checked-out frames and select
  registrations.
- The payload's ownership/freeze/copy snapshot and retained-byte accounting are
  established before the producer can remain pending. A suspended producer may
  not retain an unaccounted mutable graph whose eventual contents differ from
  the requested send.
- The "mailbox full?" observation and waiter registration share one admission
  linearization domain with slot release. A check-then-register design is
  invalid: capacity becoming free between those two operations must not leave a
  producer asleep forever.
- Releasing a USER slot either returns that credit to the free pool or transfers
  it directly to one already-admitted producer waiter. A transferred credit
  stays charged during handoff so a racing synchronous sender cannot steal it.
- Cancellation may withdraw a producer only **before** credit transfer commits.
  Once transfer wins, ownership admission is irrevocable; a later cancellation
  detaches observation rather than pretending an accepted message was rolled
  back. Any failure before envelope publication returns/transfers the credit
  exactly once.
- Waiter cancellation, STOP, graceful sealing and actor failure detach pending
  registrations atomically. Future settlement and continuation wakeup occur
  only after mailbox/accounting/lifecycle locks are released; runtime callbacks
  may re-enter actor APIs and therefore cannot run under those locks.
- A pending producer that retains target-owned payload bytes or a transferred
  mailbox credit is a physical transport loan. Actor DONE, arena/heap release
  and code-generation reclamation must remain fenced until that loan either
  publishes an envelope or returns all retained resources exactly once.
- An actor holding the serial logical lifetime must not await capacity in its
  own mailbox when only that same lifetime can release the required credit.
  Detect this dependency and fail with a structured would-deadlock diagnostic.
- A mailbox credit acknowledges **admission**, never handler completion,
  network flush, peer receipt or physical quiescence. Those require their own
  drain/reply/ACK/fence protocols.

These rules are observable semantics. Java may implement them with actor-owned
bounded waiter descriptors and LLVM may use generation-fenced scheduler
continuations/port credits; neither backend may expose pthread/JVM blocking,
poll/yield retry loops, or unbounded waiter retention as equivalent behavior.

A long-lived WebSocket/TCP stream can hold a serial actor's only lease
indefinitely. The recommended design is a **connection actor** per stream
with independent bounded queues; a serial coordination actor handles
short messages/commands. Do not bypass the gate by dispatching WebSocket
events on an uncontrolled host callback thread.

## Unified network streams

Network transport types are **adapters**, not unrelated stream systems:

- HTTP request body: `ReadableStream<Bytes>`; response body:
  `WritableStream<Bytes>`. Request/response metadata and headers remain
  typed HTTP values, outside the raw byte stream. An HTTP/2 stream has its
  own flow-control and cancellation state independent of sibling streams.
- TCP: `DuplexStream<Bytes, Bytes>` with ordered bytes and independent
  read/write half-close. TCP has **no message boundaries**; framing belongs
  to an explicit codec (`lines`, length-prefix, etc.).
- WebSocket: `DuplexStream<WsFrame, WsFrame>`, preserving opcode, frame
  boundaries, close handshake and ping/pong. Messages spanning frames need
  bounded reassembly. Binary/text validation happens at the adapter.
- Server-sent events and other push interfaces:
  `Observable<Event>` as a *view* of a single stream or explicitly bounded
  broadcast, not a second transport queue.
- `pipe`, `map`, `filter`, `tee`, `merge`, `select` and composition
  execute as Oreslang continuations with declared ownership effects.
  `tee` is not atomic fanout unless its contract expressly provides it.

Every transport advertises message/frame limits, byte limits, deadlines,
read/write directions and capabilities. Backpressure must propagate through
the whole graph to the OS adapter; OS write **admission** is not flush,
remote receipt, or peer application processing.

## Admission-domain isolation

A bounded Channel implementation must not make one unrelated runtime/context
consume another runtime's normal retained-select budget. Source Channels and
their select registrations belong to an explicit runtime/execution admission
domain with finite pending-selection and pending-arm quotas.

The implementation may still use a process-wide arbitration coordinator for
atomic winner semantics, but it should retain a larger finite process emergency
ceiling only as defense in depth. Ordinary domain A saturation must not deny
domain B merely because both share a JVM/process coordinator. A multi-Channel
atomic select is same-domain by default; crossing domains requires an explicit
verified bridge or fails closed. Raw host/interop Channel handles must prove
they belong to the receiving source context's domain (or traverse such a
bridge) before guest read/write/select/close authority is exposed.
Future-only select must still be charged to
the caller's domain rather than escaping accounting because it has no Channel
arm. Runtime-owned actor infrastructure (mailbox USER/CONTROL lanes, actor
output streams, group Mailman outboxes, event-bus subscriptions, and equivalent
backend-internal Channel users) must be charged to the owning runtime/domain,
not silently to a process host-default bucket. Source Channels created inside
an actor turn are actor-local capabilities (raw Channel handles do not cross
actor mailboxes) and must receive an actor-local normal admission budget rather
than sharing the entire context/runtime budget with sibling actors. Actor
termination fences that source domain outside actor lifecycle locks; physical
actor finalization waits for the fence. A graceful guest teardown retains the
domain through its final teardown turn and fences it immediately afterward.
Domain teardown atomically
detaches/fails its pending selections and refunds both domain and process
accounting exactly once. Structured channel-specific terminal publication
happens before a residual runtime-domain fence so shutdown does not replace a
more precise EOF/error/actor-termination result with a generic domain-close
diagnostic.

Ready operations retain the existing progress rule: a transient registration
may probe/commit an immediately ready buffer/rendezvous before retained-pending
quota rejection. Admission domains are accounting/authority boundaries, not
one-thread/one-executor-per-Channel scheduling domains.

## Terminal ordering, fairness and trust

1. A stream/channel completes with exactly one terminal outcome:
   clean EOF, error, or cancellation. Already committed channel buffer
   entries are drained before normal close; error causes are preserved.
2. Cancelled pending reads/writes/select losers detach atomically; a late
   completion must not resurrect delivery, execute a guest callback on an
   I/O thread, or refund the same credit twice.
3. Readable streams have exactly one active reader lease; no release during
   an outstanding read. A new reader cannot use a stale lease.
4. Bounded byte quotas include queued and *checked-out/unacknowledged* payloads;
   credit returns only after handler / device / transport ownership fences.
   Pending producer registrations also need finite quotas. A domain-local
   generic Channel may retain a value only while that value remains charged to
   its owning heap/arena; a stream, mailbox or transport adapter that copies,
   freezes or serializes the value must charge the copied bytes to its own
   finite budget before retaining the producer registration.
5. FAIR select rotates deterministically; PRIORITY and RANDOM require
   explicit selection. A full waiter quota may reject a pending registration,
   **but cannot prevent an immediately ready rendezvous from committing**.
6. Untrusted actors cannot receive a privileged socket/file capability or
   raw channel. Trusted isolated actors receive data copies or sealed,
   explicitly allowed handles, not host references or cross-actor shared
   mutable queues. Close and capability revocation are incarnation-fenced.
7. Source-level methods must not depend on Java-specific host classes or
   expose native pointers. A backend's lack of an implemented intrinsic is
   a compile-time unsupported-feature diagnostic, not silent emulation.

## Executable conformance gates (required before release)

- Channel: cap=0 rendezvous, cap>0 FIFO, multi-select fairness, close/EOF,
  loser preservation, pending registration quotas, cancellation/close races,
  immediate progress while quota saturated.
- Actor: serial mixed HTTP/message FIFO, one active lease across suspended
  cleanup, no pending user queue, try-send overload, self-send prevention,
  stop/restart fencing, no carrier parking and exactly-once credit return.
- Streams: exclusive reader handoff, no double-delivery or subscriber stealing,
  bounded multicast policy, cancellation during producer registration,
  no guest continuation on host I/O completion thread.
- Transports: HTTP body cancellation and streaming response close, TCP
  half-close and byte framing, WebSocket fragment/close race, burst
  backpressure and process shutdown, all within per-actor and per-runtime
  memory quotas.
- Execute the **same public `.ores` fixtures** on Graal and LLVM, with
  identical success traces or intentionally shared diagnostics. Build the
  real guest compilation paths; native unit tests alone are insufficient.

## Current integration boundaries

Java main has `ChannelRuntime` and actor-channel mailboxes but no shared
serial HTTP/message gate wired through the full transport lifecycle on main.
Stream/Observable and stdio changes are spread across draft PRs #210, #300,
#467, #538, #540, #656; reconcile their single-reader, subscriber, stdio
and actor-lifetime semantics before merging.

LLVM main has `BoundedActorPort` and `NativeActorMailbox`, not source-level
`Channel<T>` / `Observable<T>` / network-stream parity. The native
`NativeActorMailbox` with capacity 1 is a **nonqueueing try-admission gate**
that immediately rejects an overloaded entrant; higher capacities can wait
on `pthread_cond_wait` and must not be used as a guest carrier-suspension
implementation. The native serial lifetime draft is PR #195.
