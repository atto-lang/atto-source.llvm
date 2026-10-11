# Actor teardown and uncaught-failure hooks — cross-backend contract (RFC)

**Status: proposed source-language semantics, not an implemented feature.** This RFC does not make \`tear_down\`, \`on_throw\`, \`on_raise\`, \`throw\`, \`raise\`, or \`panic\` executable in either compiler. Keep fail-closed parser admission until the runtime and compiler proofs below exist.

This contract applies to Java/GraalVM and native LLVM actors. It complements \`ACTOR_CLASS_LIFECYCLE_DESIGN.md\` (Java), \`EXCEPTION_EFFECT_ADMISSION.md\` (Java), and the LLVM \`ActorArena\` / \`ExceptionPolicy\` substrates. Actor-local failure hooks and *supervisor* handlers are distinct authorities.

## Intended actor surface

\`\`\`ores
define actor M as
    @Implements
    on_start() { }

    @Implements
    receive(Message m) { }

    @Implements
    on_throw(error: ThrowError) {
        // Invoked only if an ordinary throw escaped matching try/catch/trap.
        // Must explicitly choose to recover, stop, or escalate.
    }

    @Implements
    on_raise(error: RaiseError) {
        // Invoked after an unhandled explicit raise.
        // This is NOT an ordinary catch handler.
    }

    @Implements
    tear_down() {
        // Graceful shutdown. Close sockets/streams, flush bounded buffers.
        // Future/suspending teardown needs scheduler-managed lowering.
    }
end
\`\`\`

These are reserved, **optional** actor lifecycle hooks; when implemented, \`@Implements\` must resolve to a compiler-known signature rather than a regular callable method. Typed failure arguments are the preferred canonical spelling; a zero-argument shorthand requires an explicit, typed implicit failure context and cannot silently discard failure evidence. \`on_throws\`, \`on_recover\`, and any user-defined \`on_panic\` are not alternative spellings. There is deliberately **no actor-local \`on_panic\`**.

## Failure classification and routing

| Trigger | Ordinary in-turn handler | Actor-local fallback | Supervisor |
| --- | --- | --- | --- |
| \`throw E\` | Matching typed \`catch\` / \`trap\` first | \`on_throw(ThrowError<E>)\` only after the failure escapes the turn | Receives it if no fallback or fallback escalates/fails |
| \`raise E\` | Deliberately bypasses ordinary \`catch\` / \`trap\` | \`on_raise(RaiseError<E>)\` once, at actor's turn boundary | Receives it if no fallback or fallback escalates/fails |
| \`panic E\` | **Never** | **Never**, regardless of declarations | **Always, immediately via reserved CONTROL path** |
| Runtime cancellation / isolated force-revocation | Normal safepoint unwind only when cooperative | No \`on_throw\` / \`on_raise\` | Runtime/supervisor owns termination |
| VM/process-fatal condition | Not recoverable by actor | Never | Host/process-fatal authority |

All defers/finallys belonging to a normally unwound turn run **before** the actor's \`on_throw\` / \`on_raise\` fallback. These handlers run on a *single-flight actor-local failure turn*, not on the parent supervisor's thread, not inside an unrelated message callback, and never concurrently with another actor user turn. They are admitted from a bounded reserved control/failure lane that cannot be starved by ordinary mailbox traffic.

A fallback may record/report the failure and must make an explicit, compiler-checked disposition: **recover for subsequent mail**, **stop gracefully**, or **escalate**. Recovery does not resume execution at the original \`throw\`/\`raise\` site, repeat a partially applied message, or retry a failed I/O operation. Effects remain typed and subject to compiler-enforced exhaustiveness. If the hook itself throws/raises, the runtime **escalates** instead of recursively invoking the hook. Hook errors are retained as suppressed diagnostics where appropriate; the original failure must not disappear. No implicit successful recovery from returning \`void\`/falling through.

A panic bypasses all actor-local handlers and any user-supplied \`recover\`. The runtime must enqueue a typed immutable notice directly to the *supervisor's reserved CONTROL lane* without waiting for the ordinary mailbox or bounded user output. Give supervisor notices dedicated capacity, priority, and a bounded overflow/fail-closed strategy; no actor can forge, consume, rewrite, or starve its own panic notice. Exactly-once delivery/observation is guarded by actor incarnation and terminal-event sequence. The supervisor—not the failed actor—chooses restart/escalation/stop. A child panic must not by default crash the unrelated parent or process; the process-fatal policy is an explicit host/supervisor decision.

**Panic does not run user \`tear_down\`, \`endWithCleanup\`, \`on_throw\`, or \`on_raise\`.** A panicked actor may have corrupted its invariants; no further guest callback should run. Only trusted runtime destructors, revocation, resource closing, and actor-heap reclamation may run on that path. Failure notices must not retain actor heap references or mutable objects.

## Shutdown and resource ownership

Lifecycle (logical): **starting → ready/running → mailbox-fenced → graceful-teardown → quiescent → reclaimed → done**. Panic/force kill branches from running directly to **fenced → trusted cleanup → quiescent → reclaimed → done**.

1. A stop request seals user-mailbox admission immediately; an already admitted turn may finish at a cooperative boundary. Queued but not started user mail is released without guest execution.
2. Structured children are cancelled or joined under a bounded policy. Deferred callbacks, timers, task continuations, FFI loans, message/output buffers and select/channel registrations are fenced so they cannot revive the actor.
3. If initialization succeeded and the termination is cooperative, \`tear_down()\` runs **at most once**, actor-affine, with actor state still accessible. It also runs for a host/supervisor graceful stop, not only \`self.end()\`. Its async variant must release the physical carrier on every await; a hard deadline and cancellation fallback prevent immortal teardown. No new ordinary \`receive\` turns run during teardown.
4. For compatibility with today's \`self.endWithCleanup(|| -> ...)\`, the registered synchronous cleanup action, if any, runs **after** \`tear_down\` as the **last guest action**. Both callbacks have exactly-once admission. Each failure is recorded; failures must not strand physical cleanup. If initialization never completed, use trusted initialization rollback instead of dispatching a guest method into partially initialized state.
5. Trusted runtime finalizers release owned files/sockets/handles/capabilities, revoke tokens and stale handles, clear actor-owned roots, release all memory-domain accounting, and return arenas/regions **only after** active turns, borrows, continuation pins, and native/FFI callbacks are quiescent. Never free an arena while a live loan can still dereference it.
6. Actor \`done\` settles only after *physical* actor resource cleanup and dispatcher/carrier exit. Teardown failure settles \`done\` exceptionally, with failure precedence and suppressed errors preserving the original cause.

Allow the guest to *help* close external connections gracefully; still register every owned OS resource in a trusted, runtime-owned cleanup registry so crashes, cancellations, failed startup and forgotten hooks do not leak them. Each resource is closed at most once; cleanup is idempotent and no arbitrary guest code runs under a scheduler/supervisor lock.

Memory semantics are **backend-specific but observationally compatible**:

- **LLVM:** actor-owned typed allocations should live in a unique per-incarnation arena/region (with separately tracked external OS resources), no tracing GC. Destructors / drop glue run in a safe order when permitted, and physical arena free occurs only after all loans and pinned continuations have retired. Current native \`ActorArena\` is a host-only partial substrate, not yet compiler-lowered general class storage.
- **Java/GraalVM:** actor-owned native regions and accounting can be released deterministically after quiescence, and actor graph roots/mailboxes/continuations must be dropped. **JVM heap objects are only eligible for GC**, not guaranteed to be physically freed at a deadline; foreign strong references/FFI can retain objects and must be prohibited or tracked by the ownership discipline. Do not claim 100% deterministic JVM heap reclamation.

On panic, no graceful guest teardown is promised: the supervisor/runtime must hard-close or revoke native resources even when the peer would prefer a clean protocol shutdown.

## Compiler and runtime gates before accepting source hooks

1. Reserve \`tear_down\`, \`on_throw\`, \`on_raise\`, \`on_panic\` in actor scope. Reject \`on_panic\` **permanently**. Fail closed for the other names until actual dispatch and effect typing exist. \`@Implements\` must check exact signatures, uniqueness, actor affinity, and effect restrictions.
2. Lower typed \`throw\` / \`raise\` and fail-stop \`panic\`; propagate through \`try\`, \`trap\`, \`defer\`, and continuation unrolling. Install unhandled-failure dispatch at the **actor turn boundary** (not every function call).
3. Extend ActorRuntime/source actor executor and native scheduler to drive once-only actor-affine teardown from **all cooperative stop paths**, including external/supervisor stop. Preserve \`self.endWithCleanup\` as the final synchronous action.
4. Add reserved high-priority supervisor CONTROL notices with strong sender identity, bounded payloads, no object references, fairness proof and load tests; never route panics through user inbox/outbox.
5. Prove resource ownership and physical-quiescence ordering (active turn, children, wakers, FFI loans, drains, borrowed arena handles, outputs, \`ready\`/\`done\`) and fail-closed force cancellation.
6. Run **identical Oreslang fixtures** through both Java/GraalVM and LLVM, asserting: startup/teardown order; graceful socket close; external stop; duplicate stop; cleanup failure; on_throw with/missing matching catch; on_raise bypassing catch; hook failure escalation without recursion; panic bypassing every guest hook; saturated user inbox and priority CONTROL; cancellation; restart with a new incarnation; stale handle rejection; native ASan/UBSan/leak checks; no carrier parking while awaiting async teardown.

### Verified current implementation boundaries (2026-10-11)

- Java/GraalVM source actors already implement \`on_start\`, \`receive\`, \`self.end\`, and synchronous \`self.endWithCleanup\`; Java parser currently rejects actor \`on_throw\` / \`on_raise\` / \`on_panic\` pseudo-handlers and typed exceptional-effect expressions. A first safety reservation for \`tear_down\` should reject it as an ordinary method until lowering exists.
- LLVM has host-only \`ActorArena\`, native actor-group lifecycle and native exception-policy helpers. These are **not proof** of source-level failure hooks or general-typed actor arena allocation. Partial source actor execution should not be conflated with the full contract above.

**Merge rule:** do not remove fail-closed admission or advertise semantic/feature parity until all executable cross-backend gates pass.