# Actor ↔ Supervisor typed call/reply with generational slots (v1 proposal)

**2026-10-20 — normative design target, not a claim of executable source parity.** This is a shared Java/GraalVM and LLVM design for local actor mailbox RPC, not a new networking protocol. The same caller-owned completion primitive can be adapted to an authenticated proxy transport.

## Decision

Do **not** generate a UUID for each local call. Use a **runtime-owned, bounded, caller-affine pending-call slab**, indexed by an integer slot and protected by a monotonically advancing per-slot generation. A call produces a typed `Future<R>` and a one-shot runtime-owned `ReplyPort<R>`. The port is a restricted capability, **not** an ordinary guest class, raw pointer, global request ID, or serializable credential.

The actual immutable correlation identity is:

```text
{ runtime_id, caller_endpoint_id, caller_incarnation, slot_index, slot_generation }
```

For in-process reply delivery, `ReplyHandle = (slot_generation << 32) | slot_index` is a compact 64-bit **lookup key scoped to the authenticated caller endpoint/incarnation**. No per-call UUID, clock read or random generation. A separate 64-bit endpoint/session generation fences restarts/reconnects; local runtime may represent these fields out of band, rather than copying them into every message.

The global runtime must authenticate/capability-check the endpoint independently of any integer fields. Handle guessing must not grant the ability to complete a future, read a request or impersonate a supervisor.

## Stable source-level API

Preserve the canonical existing `ActorRef<TActor>.request(T): Future<R>` and the single `run(T): R` / `run(ActorMail<T>): R` unary handler dispatch. An optional `.call(...)` alias may be added once the compiler and runtime backends are ready, but it must not fork protocol semantics.

For supervisors introduce a **separate explicitly exported application-call endpoint** `SupervisorRef<TService>.request(Q): Future<R>`; ordinary `on_panic`, child-state events, restart commands and other privileged CONTROL callbacks are **not** exposed as normal caller-selectable RPC methods. A child cannot fabricate CONTROL by calling the supervisor's application endpoint. Calls from supervisor to child use the same `ActorRef.request` mechanism. No automatic child capability is conferred just by knowing the supervisor's numeric ID.

Proposed source examples (not guaranteed to parse today):

```ores
define unary actor Worker as
    run(GetStats req): Stats {
        return Stats(/* actor-owned snapshot */);
    }
end

define supervisor actor Manager as
    // Separate, explicitly exported application request surface;
    // privileged CTRL callbacks remain non-callable by ordinary actors.
    app_run(GetPolicy req): Policy {
        return Policy(/* bounded public policy snapshot */);
    }

    on_panic(ChildFailure failure): void {
        // privileged CONTROL handling, never dispatched through app_run
    }
end

// Supervisor -> child:
const Future<Stats> stats_future = worker_ref.request(GetStats());
const Stats stats = await stats_future;

// Child -> supervisor, only when holding authorized SupervisorRef<PolicyService>:
const Future<Policy> policy_future = supervisor_service_ref.request(GetPolicy());
const Policy policy = await policy_future;
```

`app_run` and `SupervisorRef<TService>` are **design notation**, not accepted syntax. The compiler should ultimately generate a distinct app-call dispatcher separate from the supervisor's CONTROL dispatch, with no guest-accessible reflection or direct foreign actor method invocation.

For several request classes with different response classes, a future **static request→response protocol map** is preferable to returning one untyped union. Compile `Call<Req, Resp>`-style declarations into an exact match/dispatch table keyed by nominal message type and protocol version. Never permit ambiguous overloaded handlers or runtime reflection to select a reply type; the existing single effective unary `run` remains the baseline.

## Internal call transaction

1. Verify the caller owns an authorized `ActorRef` or exported `SupervisorRef`; validate requested input/output protocol, isolation domains and budgets **before** copying or enqueueing payload.
2. Atomically reserve a bounded pending slot on the caller's runtime side, with a new generation, expected static reply type/schema/version, callee identity+incarnation, deadline and cancellation state. Publish a nonblocking `Future<R>`.
3. Enqueue `CALL` with the payload and a **VM-minted opaque ReplyPort<R>**. The callee never sees a writable pointer to the pending table, raw transport handle or forgeable correlation integer.
4. Execute the callee's single admitted unary invocation or supervisor application-call turn. Its `return R` is the **only** successful reply path. Convert throws/rejections to a typed failed completion; unrecovered raise/panic follows supervision rules and fails affected pending calls.
5. The VM routes `ReplyPort.complete(result)` back to the original caller's slab. Check endpoint/runtime incarnation, callee incarnation, slot generation, schema and terminal state; exactly one of success/error/cancel/timeout wins via CAS.
6. Resume the caller's `Future<R>` on its logical execution context/actor scheduler, never inline reentering actor code on the responder carrier. Retire slot and release mailbox/byte quotas exactly once.

The internal reply port is bound to the **caller's slot, callee and expected R**. A supervisor cannot mint a success on behalf of its child by only possessing a numeric ID; existing middleware response provenance restrictions continue to apply. Any supervisor interception must satisfy the middleware authority policy independently of this transport.

### Slab, generation and lifecycle

- Use direct indexed lookup, freelist, bounded capacity and atomics; no UUID generation or global hash lookups. The runtime can own the slab independently of actor-private heaps, preserving actor-private allocation domain isolation and safe continuation lifetimes.
- A 32-bit slot index + 32-bit generation yields an 8-byte local key. On per-slot generation exhaustion, **retire the slot or rotate the entire caller incarnation after quiescence**; silent generation wrap is forbidden. Caller incarnation never repeats while stale replies can be delivered.
- A callee restart/hot reload changes callee incarnation. A call bound to an old incarnation is failed or explicitly replayed by application policy with a fresh slot and idempotency key. Never automatically complete a new incarnation's future with an old incarnation's response.
- Cancellation, deadline, caller stop, callee stop, panic and supervisor/group shutdown resolve every pending future exactly once and release all admission credits. A late reply is discarded without touching a reused slot.
- Bounded caller-side pending slots, callee in-flight admission, bytes, priority fairness and timeout handling apply to both actors and supervisors. Reserved supervisor CONTROL capacity is not borrowable by ordinary application calls.
- Reentrant or cyclic synchronous waits may deadlock serialized actor/supervisor turns. Suspension must release carrier threads, but does not make a dependency cycle safe: enforce deadlines, avoid supervisor CONTROL↔child call cycles and add runtime wait-cycle diagnostics/rejection where possible.

### Remote/proxy compatibility

The local 64-bit lookup key is **not a universal wire identity**. Existing unary TCP/WebSocket work already uses a session ID, connection generation and monotonic request ID. Preserve authenticated connection/session binding, replay prevention and per-session quotas when bridging a proxy request; do not serialize local ReplyPort internals. A local proxy adapter maps authenticated wire session/request keys to local generational reply slots. The wire request ID is a positive per-session counter, not a new UUID.

Exactly-once **local terminal future completion** is guaranteed. Exactly-once distributed request execution after reconnect/retry is **not** implied; provide an explicit application idempotency key when the caller needs it.

## Backend integration

- **Java/GraalVM:** replace per-request UUID construction in draft `ActorUnaryReplySlot` / typed middleware bridges with runtime-side slab handles after preserving one-shot admission, cancellation and actor-incarnation/provenance checks. Do not mutate production source API before compiler-supported unary dispatch exists.
- **LLVM:** use a stable runtime-owned slab and generation-checked slot lookup rather than holding native stack addresses across await or code replacement. Worker-carrier migration must not change logical caller ownership.
- **Both:** one portable result/error mapping, static message/response identity, test manifest and ABI contract; no `Future<Future<R>>`. Keep untrusted/private/shared isolation checks before enqueue and on completion.

## Conformance matrix — all unverified until executed on exact branches

1. 10k+ concurrent local calls with out-of-order replies complete only the matching typed futures; no cross-owner leakage.
2. Actor→supervisor app RPC, supervisor→actor RPC and actor→actor RPC share the same slab path; no ordinary request is admitted as CONTROL.
3. Reused slot with stale generation rejects late reply; caller/callee restart and hot upgrade reject old-incarnation reply.
4. Two simultaneous terminal success/error/cancel/timeout attempts publish precisely one outcome and retire all credits once.
5. Wrong type/schema, wrong actor or supervisor authority, wrong runtime, forged slot, untrusted payload and invalid reply port all reject.
6. Bounded pending-slot count/bytes and queue pressure reject without guest execution or partial allocation leak.
7. Cancellation before dequeue and during an awaiting unary handler do not run detached continuations or complete twice.
8. 32-bit generation exhaustion and 64-bit per-session counter exhaustion are fail-closed, not rollover/ABA.
9. Supervisor CONTROL events remain prioritized and quota-reserved during ordinary app-call storms.
10. Nested waiting cycles produce bounded timeout or explicit cycle rejection; scheduler carrier threads are not blocked.
11. Cross-process proxy retry/reconnect replay fencing, per-session monotonic IDs and explicit application idempotency boundaries.
12. JDK/GraalVM/JVM native-image and LLVM sanitizer/memory/race tests: compare throughput, p50/p99 latency, allocations/call and pending-table peak versus UUID-backed draft.

## Compatibility and implementation status

Existing canonical design: `docs/ACTOR_UNARY_STREAM_TRANSPORT_API_V1.md`. Java draft PR #984 uses UUID in a host-only reply slot, while Java draft PR #1493 already uses monotonic **per-session** request IDs for TCP/WebSocket. They solve different layers: migrate local correlation to caller-owned generational slots without weakening authenticated wire session identity. This document is a proposal only; it is not evidence that source-level supervisor application RPC or generic typed protocol dispatch currently compiles.
