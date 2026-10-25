# Oreslang actor hooks and HTTP response ownership (cross-backend contract)

**Status: design + incremental Java/Graal host source startup and response-relay
prototypes, NOT complete Oreslang guest protocol or LLVM source parity.**

## Minimal actor lifecycle hook set

| Surface | Caller | Guarantees |
|---|---|---|
| `@Implements on_start()` | Runtime on actor's exclusive logical turn | Once per actor generation, after field initializers, before READY and any receive |
| `@Implements receive(Message m)` | Runtime upon typed, serializable `ActorRef<T>.send` | Only inbound user-data entrypoint |
| `@Implements on_throw(...)` | Actor's own runtime after unhandled ordinary `throw` | Optional, once for an escaping invocation, no resume of failed instruction |
| `@Implements on_raise(...)` | Actor's own runtime after unrecovered `raise` | Optional; not an implicit catch/recover for PANIC |
| `@Implements tear_down(...)` | Runtime special cleanup CONTROL turn | Optional; at most once, bounded, only when state is safe |
| `on_child_throw/on_child_raise/on_child_panic` | Parent actor's reserved supervisor CONTROL lane | Optional child supervision, never self panic interception |
| `on_child_cancel/on_child_host_failure` | Parent actor's CONTROL lane | Optional; policy defaults require explicit configuration |

Do NOT add `on_idle`, `on_suspend`, `on_resume`, `on_message_sent`,
`on_http_request`, `on_socket_write`, `on_disconnect`, `on_timeout`
as magic callbacks. Use typed request/control messages and explicit Future
completion/stream backpressure. No remotely callable actor instance methods
are introduced; other actors use only `a.send(infer struct {...})`.

Java/Graal guest already has synchronous `on_start()` in
`OresEvalRootNode.initializeActor` before READY; this is distinct from
the new optional Java host `ActorStartupHook` for host-defined behaviors.
Do not call both for the same actor incarnation. Synchronous startup may
initialize sockets if authorized, but may not block a native carrier.
Async `on_start` must eventually lower to scheduler-owned Future barriers:
READY settles only after all startup continuations commit, with a timeout,
generation fencing and failed-startup supervisor notice. Until then, fail
closed on source startup suspensions. LLVM source `on_start` must not be
declared supported until its actor compiler emits this exact lifecycle.

## HTTP/SSR/JSON: one semantic response API, two transports

An actor receives a **typed serializable** request. One actor may handle
multiple requests; response identity is request-scoped and generation-fenced,
never derived from a raw numeric socket FD supplied by a guest.

### Trusted direct response writer

The host binds a short-lived `ResponseWriter` capability to a specific
actor/request/connection-generation. It permits `write_headers`,
`write_chunk`, `finish`, `abort` and nonblocking `await writable`.
For SSE, WebSocket and raw TCP, offer specialized capabilities with their
own framing/upgrade rules; do not smuggle upgrades through generic HTTP
headers. The writer may hold the actual OS descriptor in the trusted host
but should not expose a raw unscoped int FD to guest source. The capability
is revoked on request end/cancel, actor teardown and connection recycle.
Direct writes must not occur inside an outstanding exclusive `handle_state`
loan unless the effect checker proves safe suspension/loan fencing.

### Untrusted supervisor-relay response writer

The untrusted actor never receives a socket, FD, JNI/FFI pointer, local
OutputStream, or raw host callback. It calls `self.send(...)` on the existing
data-only bounded `ActorRef.outputs()` transport. The parent reads that
actor-authenticated stream; **metadata** actor identity and output sequence
come from `ActorOutput`, not fields supplied by guest code.

Wire frames are closed typed shapes equivalent to a `@Serializable` variant:

```ores
define pattern HttpResponseFrame as
    HttpHeaders | HttpTextChunk | HttpBytesChunk | HttpEnd | HttpAbort
end
```

The current Java host prototype uses Map values for those variants, until
the compiler supports typed, sealed discriminated payload encoding:

```json
{"request_id":"req-7","kind":"headers","status":200,"headers":{"content-type":"text/html; charset=utf-8"}}
{"request_id":"req-7","kind":"chunk","body":"<main>Hello</main>"}
{"request_id":"req-7","kind":"end"}
```

For JSON simply use `application/json` and emit UTF-8 JSON text chunks.
Binary is `{"kind":"bytes","request_id":"req-7","data":[0,255,65]}` in
the bounded prototype; production should use a canonical byte-buffer
serialization rather than giant integer lists. Headers are strict lowercase,
allowlisted and CR/LF/NUL-free. No guest `Content-Length`,
`Transfer-Encoding`, `Connection`, `Upgrade`, proxy or hop-by-hop
headers. HTTP framing belongs solely to the supervisor.

One transaction: exactly one final HEADERS, zero or more TEXT/BYTES frames,
one END or ABORT. No body for 204/205/304. Correlate every frame to the
parent-granted opaque request id; reject duplicate/stale sequence numbers,
unexpected extra fields, cross-actor payloads, frames after terminal state,
oversized chunk, and total response byte quota overflow.

The supervisor owns a **credit window** corresponding to downstream
buffer capacity. The untrusted actor must not be allowed to enqueue
arbitrary response bytes while the client is slow. The Java
`SupervisorHttpResponseRelay` prototype enforces a supervisor-granted
byte budget on acceptance; future language-level `await response.write`
must wait for credits without occupying a physical carrier. Credits are
not transferable between request ids, actors or generations. An outgoing
output-channel overflow currently fails closed; it is not yet smooth
asynchronous backpressure.

### Example Oreslang intent (not fully compiler-enabled)

```ores
define actor PageWorker<messages=PageRequest> as

    @Implements
    on_start(): void {
        // Load immutable route templates once per actor generation.
    }

    @Implements
    async receive(Message m): void {
        match m {
            PageRequest.Home(req) => {
                // output is request-scoped; direct writer on trusted actors,
                // serialized supervisor relay on untrusted actors.
                const out = rt.http_response(req.request_id);
                await out.headers(200, {"content-type": "text/html; charset=utf-8"});
                await out.write("<main>Hello</main>");
                await out.end();
            }
            PageRequest.Api(req) => {
                const out = req.response;
                await out.headers(200, {"content-type": "application/json"});
                await out.write("{\\"ok\\":true}");
                await out.end();
            }
        }
    }

    @Implements
    async tear_down(ctx): void {
        // Bounded graceful close, when runtime determines guest cleanup safe.
    }
end
```

**Note:** The proposed `rt.http_response(req.request_id)` resolves an
ambient request-scoped writer/proxy in the actor's own execution domain; it is
not yet compiler-enabled. The message contains only `request_id` and
immutable serializable request data. A trusted actor resolves a generation-
fenced, non-serializable direct writer; an untrusted actor resolves a proxy
that *only* emits serialized frames. No live socket is transported by
`ActorRef<T>.send`.

### Cancellation and supervision

Client disconnect / deadline belongs to the supervisor and must be delivered
via priority CONTROL or a revocable per-request cancellation ticket, not a
normal user mailbox that can be full. One long-lived actor may process
multiple HTTP requests, so request cancellation must NOT invariably kill
the whole actor. On actor PANIC, do not invoke actor's own `on_throw` or
`on_raise`; supervisor atomically terminates or aborts each in-flight
response on the connection it owns. A supervisor may choose HTTP 500/502 if
headers have not been committed; after headers are committed, it must close
or reset the stream rather than attempt a second status line. A cancelled
request must not leave credits, handles, scratch buffers, or FFI loans live.

## Verified vs incomplete

- Java/Graal source `on_start` implementation predates this work; new host
  startup hook + regression and guest startup regression added in Java PR #625.
- Java `SupervisorHttpResponseRelay` enforces per-request actor/sequence
  identity, headers, status, UTF-8/binary chunks, write credits and quotas.
  The existing actor output stream already freezes/isolates data and accounts
  for bytes. This is **host supervision plumbing**, not end-to-end HTTP socket
  integration or async credit signaling.
- LLVM parity and typed source `HttpResponseFrame` lowering are **not yet
  implemented**. LLVM/native protocol tests should be added, and both backends
  should execute byte-identical guest fixture streams before release.
- Actor `tear_down` source turn, direct host writer granting, FFI loan
  fencing, actual socket I/O and full same-generation async `on_start`
  await-lowering are outstanding release gates.
