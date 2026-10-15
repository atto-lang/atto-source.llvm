# Actor and supervisor middleware — v1 safety and API contract

**2026-10-14.** Cross-backend design and partially implemented reference infrastructure. This document deliberately distinguishes **implemented metadata / a host-only pipeline reference engine** from **unimplemented guest actor dispatch**. Annotation acceptance alone is not permission to skip middleware. The Java parser/AST can retain `@ActorMiddleware(...)`, but the type checker and actor bootstrap reject decorated actors until compiler-owned continuation dispatch is wired. LLVM native fixed-integer actor parsing also rejects the annotation.

## Placement and canonical declaration

```ores
import class NormalizeHeaders, ValidatePayload, EnrichTrace from "./middleware";

@ActorMiddleware(NormalizeHeaders, ValidatePayload, EnrichTrace)
define http actor Orders as
    receive(HttpMessage request): void {
        // Already sanitized and admitted under this actor request ticket.
        const req = request.get_request();
        const res = request.get_response();
    }
end
```

`@ActorMiddleware(A, B, C)` is a *single* ordered class-reference annotation on a concrete actor declaration. Stage ordering is **A -> B -> C -> handler** on entry, with reverse ordering after `await next(...)` completion. Duplicate stage classes, repeated decorators, unknown or ambiguous class references and unsupported inheritance/middleware composition fail at compile/link time. Annotation metadata must survive all AST transforms, serialization, imported actor ABI linking, monomorphization and native generation; an undecorated fallback is forbidden.

**Supervisor middleware must be configured by a trusted supervisor/listener/router**, not by an actor granting itself stronger capabilities. Consequently `@SupervisorMiddleware(...)` is not permitted on user actor classes. It can be exposed later on privileged routing declarations once a trusted policy compiler exists.

## Shared conceptual protocol

```ores
define interface ActorMiddleware as
    async next(
        MiddlewareContext ctx,
        MiddlewareNext next
    ): MiddlewareOutcome;
end

define interface SupervisorMiddleware as
    async next(
        MiddlewareContext ctx,
        MiddlewareNext next
    ): MiddlewareOutcome;
end
```

This is *target syntax*, not currently executable source. The names `ActorMiddleware` and `SupervisorMiddleware` identify distinct phase-restricted contracts; using a common signature does **not** mean they have the same authority.

- `MiddlewareContext`: scoped, immutable actor-message/request view; owned request ID/session generation and cancellation token; immutable typed payload; separate bounded application metadata, sanitized allowlisted headers and read-only transport headers. It never exposes an actor state reference, raw FD, socket, mutable cross-actor alias or supervisor CONTROL channel.
- `MiddlewareNext`: one-shot, non-cloneable continuation, scoped to a single logical request turn, which accepts the next immutable context. An invocation after returning or cancellation is invalid. Passing it to state/closure/actor messages is rejected. It is not a callable public actor method.
- `MiddlewareOutcome`: exactly one `Continue`, `Respond/Reject`, or `Fail` outcome. Short-circuiting means **no call to next**. A stage that calls next cannot commit a separate early response while downstream continues. Stream frames and unary reply slots must retain the same request identity.
- `async` is logical suspension; actor guest code still runs with at most one physical native execution lease. No live `using state.write(critical ...)` loan, transport writer borrow, mutex or other exclusive loan may cross a middleware await.
- Middleware completion or error must settle/release the owned request ticket, credits, response writer, auth context and cancellation handle exactly once; no detached task may continue processing after revocation.

## Two execution boundaries, non-interchangeable

1. **SupervisorMiddleware** runs before guest actor admission on a trusted control/data boundary. It may establish authenticated principal, tenant, quota, routing and signed trace/security facts. It cannot grant additional actor capabilities simply because a decorator requests them. On rejection it must avoid actor allocation and inbound mailbox credit acquisition.
2. **ActorMiddleware** runs only *after* a specific actor request has passed runtime admission, with the actor's effective isolation/permissions and scheduler-owned continuation. It may trim/normalize fields, validate application payloads, enrich bounded application metadata, and short-circuit with a typed failure. It cannot rewrite authenticated sender/recipient, sequence, reply ticket, transport generation, peer identity or reserved CONTROL fields.

Pipeline ordering: **ingress transport validation -> supervisor stages -> actor admission -> actor stages -> run/receive -> terminal reply/stream -> supervised egress**. A transport handshake, request body consumption or HTTP response writer is always bound to the same request generation.

For HTTP, preserve two separate concepts: (1) immutable incoming actual wire headers (after transport normalization), (2) writable, bounded *application metadata* that middleware may enrich. Do not permit middleware to forge/overwrite `Authorization`, `Host`, `Content-Length`, `Transfer-Encoding`, `Connection`, `Upgrade`, `Proxy-*`, `X-Forwarded-*`, runtime internal fields or response framing. Case normalization, control-character/CRLF rejection, duplicate-name handling, header counts and total byte budgets must be applied **before** mutation becomes visible to the handler.

For `ActorMail<T>`, define a typed immutable `MiddlewareEnvelope<T>` with a separate metadata map; do not pretend application headers are raw HTTP headers. Actor mail metadata must not replace runtime-owned `ActorInboxMail` sender/recipient/sequence/correlation. Stream `receive` remains `void`; unary `run` returns one owned typed reply. Middleware may reject a unary request, but must not create a second reply or convert stream EOF into a per-message return.

## Idempotence and authorized skip

Supervisor work often overlaps actor work: decoding, header casing, stripping whitespace, schema validation. Skipping actor middleware based on `X-Processed`, cookie, guest payload or other attacker-controlled metadata is **forbidden**.

A stage can be skipped only when all conditions hold:

1. The trusted supervisor has **actually completed its pre-handler middleware step** (or successfully invoked the next stage after its declared prework).
2. The runtime has kept a **private per-request completion receipt** bound to actor/tenant/request/session generation, stage stable ID, exact version/policy fingerprint and effective request mutation generation. The guest cannot manufacture, copy or transport the receipt.
3. A trusted deployment policy explicitly declares that *this* supervisor stage satisfies *this* actor stage's required semantics and effect version; a matching class name alone is not sufficient.
4. The actor middleware is marked **skip-eligible** and the request context has not been changed in a way that invalidates the proof. Auth, rate limits, CSRF and authorization checks are **never** bypassed based merely on an application-owned marker.
5. The runtime checks the receipt at actor admission / each relevant turn, and revokes it on retry, reconnect, topology generation changes, cancellation, or incompatible middleware code upgrades.

`idempotent` does not mean `safe to skip` automatically: a repeatable sanitizer may still be mandatory as a defense-in-depth check. Default is **execute actor middleware**. Fail closed on missing/invalid/old proof. The current Java host-only `ActorMiddlewarePipeline` demonstrates a private per-invocation receipt plus exact allowlisted ID/version skip, not a complete signed distributed attestation protocol.

## Implementation state and gates

Java branch:
- ClassDecl stores actor-level annotation metadata; parser accepts one `@ActorMiddleware` on actor declarations, rejects unrelated/duplicate annotations. `AnnotationExpander` and `TreeShaker` preserve it.
- Type checker resolves/checks referenced local middleware classes and demands an `async next(MiddlewareContext, MiddlewareNext): MiddlewareOutcome` shape. Because asynchronous instance middleware calls and guest actor continuations remain unfinished on this mainline, decorated guest actors fail closed. Host-side direct bootstrap independently rejects them before construction.
- `ActorMiddlewarePipeline` implements a **trusted host-only reference model** for two ordered phases, single-use continuation, immutable bounded application metadata, supervisor-issued invocation-local proof and explicit allowlisted stage equivalence. It is **not** bound to `OresEvalRootNode.receiveActor`, the actual HTTP adapter or live guest `ActorMail`; it grants no new guest source execution feature.

LLVM branch: `@ActorMiddleware` and `@SupervisorMiddleware` are rejected in native frontend pending typed decorator metadata and native execution. LLVM socket/HTTP response host experiments do not imply middleware parity.

**Required release proofs:** executable annotated unary/stream generic-mail and HTTP guest programs in both compilers; imported middleware ABI/version binding; linked graph of inherited middleware; proper `next` one-shot and cancellation before/after await; dynamic disconnect/timeout/retry/restart generation fences; falsified header/receipt attacks; UTF-8/CRLF/duplicate/header-budget tests; multiple-stream frame ordering and response ownership; AOT and hybrid; LLVM native CTest/sanitizer; exact private source CI. The shared `contracts/v1/actor-family-conformance.json` enumerates these as unverified targets.

`ProxyActor` may carry the already-authenticated request context through an approved codec but cannot carry raw middleware continuations, actor state, response writers or supervisor receipts as untrusted wire fields. Remote claims must be independently verified by the receiving trusted supervisor.
