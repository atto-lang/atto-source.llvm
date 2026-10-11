# Supervisor recovery contexts and nested actor families (v1 contract)

**Status (2026-10-11): shared design target, NOT a claim of executable guest `throw/raise/panic/recover` or native source-level supervision.** The JVM branch adds a first runtime implementation of inherited *managed lifecycle ownership* for nested spawned actors. The LLVM branch has a host-only ActorGroup and must still lower source spawn, await and supervisor CONTROL callbacks. Keep these gates separate.

## v1: flat supervision for recursive spawn

For an actor family A -> B -> C, all descendants inherit the same supervisor S and the same **lifecycle-owning ActorGroup G** from the spawning actor by default. Spawning an actor never implicitly creates a supervisor or a new group. Store `spawned_by` as lineage metadata, not a supervisory authority. Keep actors' own mailboxes, actor states and single-flight user-execution lanes separate. Underlying runtime workers/carriers may be shared, subject to private/shared/untrusted pool and isolation policies.

The root must first be admitted to S/G. The source-level default root-supervisor bootstrap is a future integration gate; don't assume that an ordinary host-created/unmanaged actor already belongs to an implicit cooperation ActorGroup. In the present JVM runtime a managed root passes *structured lifecycle ownership* to recursively spawned children, while the event-bus `members` and its unforgeable join capability remain independent and never auto-inherited.

- A's spawn of B, and B's spawn of C, do not make A/B supervisors.
- C's terminal panic is reported as a typed CONTROL failure to S, not synchronously to B or A.
- A/B/C have distinct invocation contexts and defer/recover stacks. C's failure never directly unwinds B's or A's stack.
- Existing **structured spawn lineage cancellation** remains: stopping/cancelling A can stop descendants B/C, while group close stops the entire managed group. Supervisory failure policy is a separate decision.
- A group owns managed worker *lifecycle authority*, not automatic unbounded access to communication, actor-owned state, or cross-actor cancellation. Source user code communicates with a typed `ref.send(message)`; no direct foreign actor method invocations.
- `spawn T()` returns `ActorRef<T>` promptly; `await spawn T()` is a readiness barrier and also returns the ActorRef. `await ref.get_ready()` and `await ref.get_done()` are the explicit observer method forms. DONE is after finalization, not just after a stop request.
- Group close must fence recursive admission before publishing a child, reject stale/cross-group ownership, and never complete DONE until physical finalization.

## Two independent trees

```text
supervision ownership:      S  ── owns ── G(A, B, C)
spawn ancestry (diagnostic): A  ──spawns──> B  ──spawns──> C
logical execution stacks:   A.invoke#1 | B.invoke#7 | C.invoke#4  (separate)
physical dispatcher:        shared runtime scheduler / bounded pools (actor-kind scoped)
```

## Defer/recover: same invocation, including awaited continuations

Each *admitted actor message invocation* and each *admitted supervisor callback* owns one logical ExecutionContext. Its stack of frame-scoped LIFO `defer` callbacks and its current recoverable failure **must survive `await` / carrier migration**, not reside permanently in a Java ThreadLocal/native thread_local. Carrier-local variables can temporarily point to the context during the admitted turn, but never own recovery semantics.

- Nested calls and an `await` chain preserve the same logical unwind context; a suspension does not create an independent recover scope.
- `recover()` only succeeds when directly invoked by an executing deferred function during an active recoverable unwind, consuming the current recoverable failure once. Outside that situation it returns `nil`. An ordinary helper called by a defer cannot call `recover()` on the defer's behalf.
- `throw` can be intercepted by admitted typed catch/trap or an eligible `defer/recover`. `raise` bypasses ordinary catch/trap (explicit escalation), but a directly executing eligible deferred `recover` can consume it; if neither consumes it, the respective local unhandled hook runs.
- True `panic`, cancellation, process exit and host-fatal failures cannot be consumed by `recover`. Cleanup defers still run under bounded cleanup/failure-priority rules. Child terminal panic reaches its supervisor only as a typed CONTROL event.
- After a successful `recover`, execution does not jump back into the throwing function or re-execute the failed `await`; normal function exit/unwinding completes.
- A detached task, another actor message, or child actor has its own execution context. A future can explicitly *deliver a failure value* to an awaiter and the awaiter can throw a **new local** exception; its recover scope does not rewind the remote execution.
- Supervisor `on_panic(childFailure)` is a fresh **supervisor-owned invocation** scheduled on S's exclusive logical lane. It can recover failures of its own nested awaits, but not the child's original terminal panic. If the supervisor panics, cleanup runs and a typed CONTROL event goes to S's parent; no inline parent callback from a failed child carrier.
- Supervisor CONTROL is priority/reserved, but cannot interrupt existing supervisor user code or execute reentrantly. A suspended supervisor callback may yield the physical carrier and later resume under single-flight admission.

## v1 nested spawn example (illustrative; not a claim of parsed guest syntax)

```ores
define actor A as
    receive(Message m) {
        const b = await spawn B();
        b.send(infer struct { action: "start" });
    }
end

define actor B as
    receive(Message m) {
        const c = spawn C();
        await c.get_ready();
        c.send(infer struct { action: "work" });
    }
end
```

Both B and C inherit A's managed group G. No implicit new supervisor or group; source message sending remains the only cross-actor invocation.

## Required test matrix (both backends; fail closed until executable)

1. A->B->C inherits exactly one managed lifecycle group, independently of group event-bus join membership.
2. Concurrent group close vs child spawn cannot publish an unowned child or strand an ownership reference.
3. Group close stops all A/B/C; parent subtree cancellation still cascades. Each get_done waits for physical finalization.
4. A/B/C user-turn lanes stay single-flight, though actor kinds may share carrier pools.
5. Local `defer/recover` unwinds through multiple `await` resumptions, including carrier changes, once and LIFO.
6. S receives one immutable CONTROL failure for C panic; neither A nor B can intercept it using recover.
7. S's on_panic owns an independent invocation context; its own recoverable failure may be handled locally; a true supervisor panic escalates to grandaddy.
8. Cancellation and terminal panic run cleanup but never become recoverable; ensure duplicate notification and stale identity fences.
9. Verify against the **exact source Git tree** in CI. When out of org Actions budget, mirror exact tree/blobs to a funded test org, using no private cross-org checkout secrets.

## Integration boundary

The present JVM `ActorGroup` is a **cooperation domain plus explicitly managed-worker registry**, not itself a supervisor process or dispatcher. The new inherited-ownership behavior concerns that managed-worker registry only; a real supervisor scheduler/CONTROL callback integration remains a release gate. The LLVM `NativeActorGroup` is a host-side lifecycle authority; it does not yet execute guest spawn expressions or guest recovery frames. Do not conflate tested host primitives or docs with compiler-level parity.
