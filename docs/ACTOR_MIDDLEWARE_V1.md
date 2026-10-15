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

## Typed `next_val` vs procedural `next` (2026-10-14 extension)

Do **not** spell a generic typed middleware response as `!void`. In Oreslang `!void` is a complement type, not a complete continuation or outcome protocol. A middleware method must instead declare the precise *actor reply type* `R`:

```ores
define class NormalizeOrders as
    // Value hook: one invocation either continues, returns a typed reply,
    // or rejects; the middleware does not own an HTTP response writer.
    async next_val(MiddlewareContext req, MiddlewareResponseDraft res):
            MiddlewareDecision<int> {
        if req.invalid then
            return MiddlewareDecision<int>.Reject(400, "invalid");
        fi
        return MiddlewareDecision<int>.Continue(req.clean(), res);
    }

    // Around hook: selected for streaming actors, or for unary actors
    // without next_val. The next capability is one-shot.
    async next(MiddlewareContext req, MiddlewareResponseDraft res,
               MiddlewareNext<int> next): MiddlewareOutcome<int> {
        return await next.proceed(req, res);
    }
end
```

The above is **target Oreslang source syntax**, not executable guest code. The parser/validator recognizes the middleware method signatures on the Java feature branch but rejects activated decorators until the guest continuation backend is integrated.

`MiddlewareDecision<R>` is the sum of:

- `Continue(updatedRequest, responseDraft)`: continue with bounded, immutable application payload/metadata and an optional uncommitted response draft. The **runtime** calls the next stage exactly once; a `next_val` method never receives the continuation capability.
- `Reply(value: R, responseDraft)`: short-circuit with one concrete typed `R`. The **runtime** validates ownership and encodes/sends it exactly once.
- `Reject(error/status)`: terminate the unary request without invoking the actor handler.

`MiddlewareOutcome<R>` is the completed typed `Reply<R> | Reject`, used by an around hook after it invokes `MiddlewareNext<R>` or short-circuits. For streaming actors, the procedural next hook does **not** change the `void` return type of `receive(...)`; it manages continuation/stream control in a request-scoped outcome rather than fabricating a per-message typed reply. A single middleware class may declare both hooks; select **one** according to the actor's effective role (for unary prefer `next_val`, falling back to `next`; for stream use `next`). The compiler rejects stream actors with value-only middleware.

Supervisor stages share the conceptual signatures and may `Continue` or `Reject`. A supervisor value stage may **not** produce an application actor `Reply<R>` without a trusted actor reply authority; the current host reference executor explicitly rejects this. Supervisor policy binding remains privileged and cannot be installed by actor decorator.

`MiddlewareResponseDraft` may specify bounded, validated status and headers (e.g. 201 and `Content-Type`), but is **never** a response writer, transport handle or committed HTTP exchange. In the host reference model, status 204/205/304 are forbidden for a typed body and framing headers cannot be set by middleware. The final HTTP reply codec and connection generation check belong to the trusted adapter. There is no automatic `Future<Future<R>>` nesting, no simultaneous `Reply<R>` and streaming write, and no empty/void `next_val` result.

**Implementation in Java host reference:** `ActorMiddlewareValuePipeline<R>` has two independently selectable hook kinds (`ValueHook<R>` and `AroundHook<R>`), immutable `ResponseDraft`, typed `Continue/Reply/Reject`, supervisor/actor stage ordering, single-use `next`, cancellation checks, bounded metadata through `ActorMiddlewarePipeline.Envelope`, exact identity-fenced supervisor skip receipts, and downstream-drain failure fencing. It does not attach to real Oreslang `run`/`receive` or raw network sockets; guest declarations remain fail-closed and LLVM must not report parity.

**Response-draft propagation invariant:** every `Continue(updatedRequest, responseDraft)` and `next.proceed(updatedRequest, responseDraft)` passes the explicit draft; there is no one-argument shorthand which silently resets a previously selected status or safe header. The Java reference engine has removed both shorthand methods and carries request/draft revision identity in the skip proof.

## Trusted mailbox pre-dispatch integration slice (2026-10-14)

Java ActorRuntime.ActorCodeExecutor now defines a backwards-compatible *trusted synchronous* prepareActorMail(...) hook. The ActorRuntime executes it **inside the admitted single-flight actor turn** before receiveActor(), after validating actor-owned state/code-image identity. It rejects changing runtime-owned recipient, sender, or sequence; revalidates message graphs, untrusted capabilities, mutex and actor references; and publishes the prepared envelope as currentMail() to the receiver. A null return is a fail-stop error, not an implicit pass or silent message drop.

Until native memory-loan/continuation support exists, changed payloads must be **immutable scalars** and may not exceed the byte budget already reserved for the original mailbox payload. The original envelope owns the mailbox slot and release ticket. Mutable graph substitutions, raw tensor handles, socket capabilities, header/trailer mutations, arbitrarily expanded values and asynchronous off-carrier callbacks are not authorized through this hook.

**This is not @ActorMiddleware execution**. Decorated guest actors remain rejected until the guest compiler constructs an ordered continuation chain within the actor's logical message lifetime, proves release/reacquire of state loans across awaits, and binds a single owned response writer/reply slot. The typed host middleware reference pipeline and synchronous runtime pre-dispatch hook are separate components; do not assume they are connected just because both compile. LLVM retains fail-closed annotation diagnostics.

The first runnable tests target normalization, metadata identity forgery, null and mutable replacements, oversized values, and one-physical-lane delivery. They are source regressions, not verified JUnit passes until exact-tree runners execute them.

A trusted host integrator may use ActorMailboxMiddlewareBridge.wrap(executor, orderedStages) to compose up to sixteen synchronous, immutable-scalar sanitizers. The bridge delegates initialization and receive to the exact original whitelisted code executor, runs each stage in actor-turn order, rejects any stage that changes mailbox identity before calling subsequent stages, and leaves final capability/memory verification to ActorRuntime. This adapter is not an annotation linker and does not allow guest code to register arbitrary host callbacks.
