# Unary, stream and network actor API — canonical v1 contract

**2026-10-14 — specification / integration target, not production-ready executable parity.** This is a follow-on to `ACTOR_FAMILIES_SEALED_STATE_V1.md`. It deliberately reconciles previous Java/Graal design/implementations (#464 unary `run` and `ActorRef.request`, #545–#547 transport declarations and admission, #543 `ProxyActor`, #750 reusable HTTP handlers) and the LLVM native HTTP/TCP substrates. Older branches have different implementation maturity; a merged PR into a non-main feature branch is not evidence that current `main` supports its syntax.

## Decision: separate role from wire protocol, isolation and admission

An actor has four *independent* axes:

1. **Reply role:** `UNARY` or `STREAM`; a lifecycle-only actor has no data-plane handler.
2. **Transport binding:** `ACTOR_MAIL` (default), `HTTP`, `TCP`, `WEBSOCKET`. `ProxyActor` is a distinct proxy/discovery role, not a competing `RemoteActor`.
3. **Isolation kind:** `SHARED`, `PRIVATE/ISOLATED`, `UNTRUSTED`; inheritance must preserve isolation and capability grants.
4. **Admission:** bounded `requestConcurrency` and `messageConcurrency`, with optional `serial` normalization to (1,1). Logical in-flight work is not parallel native execution of the same actor.

**Proposed canonical declaration grammar (not asserted to parse on current main):**

```ebnf
ActorDecl  = "define" ["serial"] [ReplyRole] [Transport] "actor" Ident
             [Inheritance] ("as" Members "end" | "{" Members "}");
ReplyRole  = "unary" | "stream";
Transport  = "http" | "tcp" | "websocket";
```

The isolation modifier / `isoactor` form remains orthogonal to this grammar; the final parser must normalize both modifier orders without changing ordinary identifier meanings. Do not reserve `http`, `tcp`, `stream`, or `unary` globally until lexical conflicts have been measured.

Normalization:

| Source | Reply role | Transport | Handler |
|---|---|---|---|
| `define unary actor` | UNARY | ACTOR_MAIL | `run(...): R` |
| `define stream actor` | STREAM | ACTOR_MAIL | `receive(...): void` |
| `define actor` | STREAM by default | ACTOR_MAIL | `receive(...): void` |
| `define http actor` | STREAM | HTTP | `receive(HttpMessage): void` |
| `define tcp actor` | STREAM | TCP | `receive(TcpMessage): void` |
| `define websocket actor` | STREAM | WEBSOCKET | `receive(WebSocketMessage): void` |
| `define unary http actor` | UNARY | HTTP | `run(HttpMessage): R` |

The established legacy unqualified `define actor` + **sole** `run(T): R` may continue to infer UNARY for source compatibility, with a migration diagnostic; it cannot silently switch to STREAM because there is no `receive`. For explicit role declarations, role/handler mismatch is a hard compile error. Do not infer protocol from return type alone.

`define unary tcp actor` and `define unary websocket actor` are **reserved/fail-closed**, not implied working aliases: unary TCP requires an explicit framed protocol and request correlation; WebSocket unary requires an explicit message/request-id correlation, close and timeout contract. Raw TCP reads can split/merge writes arbitrarily and are not request boundaries.

## Handler API: one VM-owned dispatch path

### UNARY: `run` is a private handler, not a callable method on an actor reference

```ores
define unary actor Multiply as
    run(ActorMail<int> mail): int {
        return mail.value * 5;
    }
end

pub async routine main(): void {
    const worker = spawn Multiply();
    await worker.get_ready();
    const int result = await worker.request(7);  // Future<int> -> 35
    worker.stop();
    await worker.get_done();
}
```

The earlier Java guest implementation used `run(T): R`, which **remains accepted** under the unified v1 contract. A `run(ActorMail<T>): R` handler is an optional explicit envelope spelling for actor-message metadata: both map to `ActorRef<TActor>.request(T): Future<R>`. The runtime creates and authenticates any envelope; callers cannot forge reply slots, sender identity, correlation IDs or generations. Do not overload `run(T)` and `run(ActorMail<T>)` in the same effective actor protocol; exactly one effective signature is allowed.

`run` must be non-public, instance-only, concrete and non-generic, one input, non-`void` result. Inputs and outputs must be owned/sendable; live Future, Channel, borrowed view, socket, response writer or actor state loan is not a serializable result. Every successful invocation completes **one** runtime-owned reply slot. `throw` may reject just that invocation, unrecovered `raise` escalates, `panic` terminates the actor and fails affected requests. A handler must not invoke/capture/override the VM dispatcher or be called directly via `self.run`, `ref.run`, `super.run` or reflection.

`async run(...): R` is a **target** source spelling for a suspension-capable unary handler. The semantic reply remains `Future<R>` at `ActorRef.request`; no `Future<Future<R>>`, duplicate completion or detached task escape. The scheduler must keep the request's logical context, reply ownership and generation across awaits and carrier migration; validated `with io, suspend` effects are required when those effects occur. Existing Java main rejects async actor instance methods; this feature must not be accepted by deleting that guard without actual continuation lowering.

### STREAM: `receive` is a private, return-`void` event handler

```ores
define stream actor MultiplyStream as
    receive(ActorMail<int> mail): void {
        self.send(mail.value * 5);    // one of zero/many outbound items
        self.send(mail.value * 10);
        return;
    }
end
```

A stream actor accepts `ref.send(T)` and may produce zero or many typed outputs via `self.send`/bounded `ref.outputs` or `self.forward`/`self.backward` when part of a typed pipeline. Returning from one `receive` turn does **not** imply stream EOF or actor termination. EOF requires an explicit terminal protocol/actor shutdown. `receive(...): R` for nonvoid `R` is rejected with a diagnostic to use `run` and `unary`. A unary actor cannot also define/inherit `receive`, and a stream actor cannot define/inherit `run`. Mixing `ref.send` and `ref.request` on one actor must fail **before** mailbox, byte credit or payload copy admission.

**Async receive:** `async receive(...): void` is target sugar for compiler-owned continuation scheduling, not a free detached Future that might outlive request/connection ownership. Source-level support must be proved before accepting the spelling. Synchronous `receive` may already reach supported approved suspension safepoints in the existing runtime; do not label that full async method support.

### Lifecycle-only

An actor with only `on_start` / supervision may have neither handler. It is not a stream receiver merely because its role defaults to STREAM; `send` and `request` fail, while `get_ready` and `get_done` work. This matters for supervisory control-plane actors.

## HTTP: typed exchange façade, not a transportable actor message

The current Java `http.handler("ActorType")` dispatches an actor `ActorMail<String>` token which it must claim using `http.claim`. The proposed `HttpMessage` is a future **compiler/runtime-minted request-scoped façade**, not evidence that `HttpMessage`, `body_to_json` or transport declarations already execute on main. The adapter must bridge the existing single-owner `HttpExchange` capability rather than duplicate socket authority.

### Streaming HTTP by default

```ores
define http actor Page as
    receive(HttpMessage m): void with io, suspend {
        const req = m.get_request();       // HttpRequestView (read-only)
        const res = m.get_response();      // HttpResponseWriter (single owner)
        const name = req.query("name");
        await res.start(200, "text/plain; charset=utf-8");
        await res.write(name);
        await res.finish();
        return;
    }
end
```

`HttpMessage` owns an opaque request/session correlation and an unforgeable scoped capability, not an ordinary sendable class. `m.get_response()` gives a **writable** `HttpResponseWriter` only for STREAM HTTP handlers. Its status/headers are single-commit, chunk writes are bounded and await backpressure, and `finish` or `abort` is terminal. Returning `void` without committing/finishing a response is a request failure (500 before headers; abort/reset after partial output), **not** implicit success. One actor can process many requests through request-scoped capabilities, with concurrency separately admitted and ownership generations fenced.

### Unary HTTP: one typed return -> exactly one HTTP reply

```ores
define unary http actor Calculate as
    async run(HttpMessage m): int with io, suspend {
        const req = m.get_request();
        const config = await req.body_to_json<CalculationRequest>();
        const myInt = await do_network_req(config);
        return myInt * 3;
    }
end
```

The exact `async` source grammar and generic JSON helper are target syntax, not current main's accepted HTTP API. The declared return `R` maps to one `HttpReply<R>` completion with a strict, versioned HTTP response codec. For a data-only scalar `int`, the default is HTTP 200, `application/json; charset=utf-8`, decimal JSON body, subject to HEAD/204/205/304 suppression rules. Unknown or unencodable return shapes fail at compile/link time; no implicit `toString` or unsafe host reflection. A specialized `HttpReply<T>` return may select status/headers/codec explicitly after validated authorization. A request body read may happen once; `body_to_json<T>` is bounded and errors on invalid UTF-8/JSON/schema.

**Single-writer rule:** A unary `m.get_response()` returns only a `HttpResponseDraft`/metadata configuration façade (status, allowlisted headers, codec), **never** a committed/chunk-writing `HttpResponseWriter`. The runtime commits the one reply only after `run` returns successfully and its data is encoded. No `start`/`write`/`finish` in unary handlers or borrowed writer from outer scope; fail at compile time, with request-generation fencing as a runtime backstop. A returned value must not produce a second write after a response was already committed. If no response exists on a failed unary turn, a trusted error mapper selects the HTTP failure before headers; after headers, close/reset, never send another status line.

For an untrusted HTTP actor, no raw `HttpExchange`, FD or writable socket handle is granted. An adapter may use a supervisor-owned typed relay with byte credits; passing an opaque raw handle across an actor boundary remains illegal.

## TCP and WebSocket: default STREAM, explicit framed events

```ores
define tcp actor SocketSession as
    receive(TcpMessage m): void with io, suspend {
        // m describes connected/data/eof/error events with generation-fenced
        // TcpConnection capabilities; all writes await backpressure.
        return;
    }
end

define websocket actor ChatSession as
    receive(WebSocketMessage m): void with io, suspend {
        // m carries typed open/text/binary/ping/pong/close/error events,
        // with strict control-frame and close-handshake sequencing.
        return;
    }
end
```

These type/event names are target contracts, not enabled type signatures in both compilers. TCP emits byte sequences, not a per-read message protocol; require framing before exposing request/reply semantics. WebSocket messages are framed but not automatically correlated RPC requests. The connection owner, reader/writer split, UTF-8 validation, fragmentation, close/ping handling, quotas, cancellation and backpressure remain transport-runtime-owned. The actor may never obtain an unscoped raw FD or send a transport capability as a normal `ActorMail` payload.

## Safety / scheduling / placement invariants

- **Separate handler families:** `run`/UNARY and `receive`/STREAM are mutually exclusive across the *effective inherited* actor class. No overload, ambiguity, accidental `void` unary, or nonvoid streaming. Untrusted and private actors have the same protocol admission guarantees.
- **Transport declarations require a real binding.** Generic `spawn HttpActor()`/`spawn TcpActor()`/`spawn WebSocketActor()` cannot be treated as a listener/session. Runtime-owned transport factory or supervisor binding supplies admitted listener, connection or request leases.
- **No FFI/pointer leaks.** `HttpMessage`, `TcpMessage`, `WebSocketMessage`, writers and response drafts are ephemeral, non-Sendable and cannot escape as actor state, return payload, closure or child message. Detached tasks may not retain exchange authority.
- **Actor state remains sealed.** Only `state { ... }` (zero/one effective schema) stores guest-owned persistent instance data; `with` declares effect upper bounds; `using state.read`/ `using state.write(critical ...)` acquire lexical loans via the sealed `self.handle_state`. No loan crosses await/select/cooperate without release and safe reacquisition.
- **Per-actor native exclusivity remains.** The logical `requestConcurrency` and `messageConcurrency` policy measures outstanding request/message lifetimes, not parallel native guest execution. `serial` means (1,1). Non-serial concurrency remains fail-closed until scheduler/ownership guards are proven end to end.
- **Reply correlation is trusted.** Exactly one terminal unary reply; generation-fenced request/exchange/stream IDs; no fake sender/sequence. Cancellation/timeout vs success race publishes at most one terminal outcome and drops/retires every reply slot/writer/buffer lease exactly once.
- **Backpressure is byte and item bounded.** No blocking an actor carrier for socket I/O, no unbounded wait queues, and no silent TCP/WebSocket/HTTP buffer growth. Errors distinguish invalid payload, timeout, disconnect, budget, actor panic and protocol mismatch.
- **ProxyActor integration:** a proxy may forward unary or streaming typed messages through an authenticated transport after protocol/codec agreement; its streaming direction and reply semantics must not be inferred from an untrusted wire payload.

## Readiness and migration gates — fail closed

**Observed Java main at 2026-10-14:** source `receive(ActorMail<T>): void` is enforced by `TypeChecker`; the HTTP server's `ActorMail<String>`/`http.claim` interface is described in `HTTP_SERVER.md`. Java #464 contains source guest unary `run`/`request` work in a prior feature lineage, but `main` at inspection did not expose the accompanying unary contract fixture as executable. Java #545–#547 transport modifier/admission syntax is on older stacked branches and did not prove that HTTP/TCP/WS actor adapters execute in current main; the pooled HTTP work in #750 is a separate draft. **Do not use prior focused CI as proof of an integrated latest-main behavior.**

**Observed LLVM main:** native TCP nonblocking substrate and HTTP response-frame guard exist; guest source actor HTTP/TCP/WebSocket plus unary reply and transport bindings are not at Java parity. C/native host tests alone do not establish the compiler surface.

Release gates, in order:

1. One canonical AST descriptor holding independent `replyRole`, `transport`, `actorKind`, `admission` plus inherited compatibility proofs; no hidden dropped metadata in annotation expansion, imported linking, tree shaking or native codegen.
2. Java parser/typechecker accepts the explicit role spellings, matches effective handler signature/inheritance, and **rejects unsupported transport or async execution** before runtime admission; source-only positive parsing is not enough.
3. Port/reconcile Java #464 request-reply scheduling onto current main without resurrecting forbidden ordinary actor fields or legacy `using` signature effects; compiled unary `run` and stream `receive` guest executions pass.
4. Adapter `HttpMessage` with strict unary JSON result codec and streaming writer ownership on Java, then LLVM; preserve the existing legacy `http.claim` API until migration and equivalence are tested.
5. TCP and WebSocket guest session adapters with typed events, proper network framing/close states, authentication/capabilities, byte credits and failure injection.
6. Native LLVM source AST, type checks, guest dispatch, async continuation and response parity. Stress both modes with malformed/untrusted frames, cancellations, timer/connection reuse, EOF, large inputs, actor panic and shutdown.
7. Run identical valid and invalid `.ores` fixtures in both backends, full Maven/JDK25, AOT/hybrid, native compiler CTest plus ASan/UBSan and exact-tree/manifest parity in authorized **private** CI. Gate maturity on observed completed runs, never extrapolate from designs.

See `contracts/v1/actor-family-conformance.json` cases ACT-033 onward for proposed checks. All remain `not_verified` until source execution tests substantiate them.
