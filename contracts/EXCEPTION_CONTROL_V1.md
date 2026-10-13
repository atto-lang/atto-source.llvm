# Oreslang exceptional control and root supervision — v1 design contract

**Status: normative target semantics, not an executable source-language parity claim (2026-10-12).**
Both backends must fail closed for exceptional source constructs that they cannot yet lower. In particular, the Java parser currently reserves `throw`, `raise`, `panic`, and `recover` rather than executing them; LLVM has host-side effect/supervisor machinery but not general guest exception lowering. Do not mark source parity complete from host unit tests.

## Three effect identities, not three names for panic

| Effect | Intent | Ordinary `catch` | `trap` callable | Explicit `recover` | Unhandled terminal category |
|---|---|---|---|---|---|
| `throw E` | Typed, expected, recoverable application failure | Matching typed catch | Converts to `None` | Not applicable | terminal `Panic(origin=Throw)` if it reaches the actor/task/root boundary |
| `raise E` | Explicit nonlocal abort/escalation | Never | Never | Eligible, authorized recovery boundary only | terminal `Panic(origin=Raise)` if it reaches the actor/task/root boundary |
| `panic E` | Broken invariant or unrecoverable guest failure | Never | Never | **Never in the panicking actor**; v1 does not admit source panic recovery elsewhere | `Panic` |

A handler or supervisor may escalate the *delivery scope* of a failure. **Required invariant: an unhandled `Throw` or `Raise` becomes a terminal `Panic` at an actor or non-actor task/root execution boundary.** This is a single boundary normalization, not an intermediate `Throw -> Raise` rewrite and not an exception rethrown through callers. The terminal category is `Panic`, but the immutable diagnostic envelope preserves `origin=Throw|Raise|Panic`, the original error type/cause and the suppressed cleanup failures. Never erase the initiating error. An already-in-flight `Panic` remains `Panic`; cancellation and host fatal termination remain separate.

`defer` and `finally` are LIFO cleanup, not exception handlers. They execute while the runtime is still executable on normal return, local catch, trap conversion, recovery, and terminal unwind. Cleanup failure is not allowed to suppress higher-priority termination or panic. Invalid native state, fatal host failure, forced kill, or process death cannot guarantee cleanup execution.

## Callable contracts: checked throws, not an assumed unchecked default

The canonical planned effect spelling is `using throw<E>`, consistent with the existing `using` effect-row proposal. A future `throws E` signature suffix MAY be introduced solely as syntactic sugar for `using throw<E>` (with deterministic format normalization and duplicate-effect rejection); neither spelling is currently established as executable source syntax. Do not introduce a second independent exception system.

```ores
pub fnc read_user(UserId id): User using io, throw<NotFound> {
    // may emit throw NotFound(...), once source lowering is implemented
}

pub trap fnc try_read_user(UserId id): User using io {
    return read_user(id);
}
// The trap call result is Option<User>; its body can throw but no ordinary
// throw crosses the trap boundary. Raise/panic/cancel still propagate.
```

Unannotated local/private callables **infer** transitive exceptional effects; they are not implicitly classified as `throws Any`. Explicit `using` is an upper bound / public contract. A public API must expose its inferred typed throw row or explicitly handle/trap it before shipping under the checked-effects profile. Unsupported or unknown FFI/dynamic calls are conservatively effectful and cannot satisfy a proven non-throwing API without a trusted effect summary. Function values, imports, trait slots, generic instantiations, overrides, callback effects, async continuations, JIT/AOT, and Wasm/native metadata must preserve the effect row.

`trap fnc(...): T` preserves its current API shape: success produces `Some(T)`, uncaught ordinary throw produces `None`. If T is itself an Option, preserve `Option<Option<T>>`. Async trap produces `Future<Option<T>>`; late post-await throws are converted at the same trap boundary. Traps must not convert `raise`, `panic`, cancellation, process exit, or host-fatal errors. A `trap` *expression* is not implied by this callable contract.

The `Assert.equal_throws` and similar source-library names are naming conventions for ordinary error-producing operations, **not signature effect metadata**. The existing `rt fail` assertion path will need a typed `throw<AssertionError>` classification before typed source exception release. Deliberate validation errors should throw; invariant failures can panic. Security-sensitive invariant checks must never be compiled away as debug-only assertions.

## `defer` / `recover` exact boundary rule

A `defer` is cleanup only; **a `recover` handler explicitly attached to an eligible deferred-unwind scope** can handle a current `Raise` effect. For a raise, unwind enters inner cleanup scopes in LIFO order, then considers the nearest eligible `defer+recover` attached to the current dynamic continuation. Ordinary `try/catch`, `trap`, and unrelated `defer` blocks never handle it. Recovery consumes the raise only after it produces an explicit valid replacement outcome for the enclosing scope; it does **not** resume at the failed program counter or re-execute a partially completed operation. The compiler must validate a recovery capability, complete return value, actor-state/borrow/lock consistency, and one-shot continuation ownership. If a recover handler itself throws/raises/panics, ordinary precedence and the next eligible outer boundary apply. A `recover` never handles `Panic`, `Cancel`, `HostFatal`, or explicit process termination.

This specifies **semantic behavior, not yet a finalized `recover` grammar**. The parser must fail closed rather than accepting ambiguous statement syntax. Recovery can complete a failed non-actor function or actor turn only when its explicit replacement is well-typed and state-safe; otherwise the effect continues outward to the next eligible boundary.

## Lexical and actor boundaries

1. An ordinary throw first seeks the nearest matching `catch` in the same dynamic continuation; unmatched catches are skipped. On escape from a `trap` callable, it becomes `None` after cleanup. Otherwise it remains a typed `Throw` and unwinds.
2. A raise bypasses all ordinary catches/traps and searches for a statically admitted `recover` boundary with the required authority. Recovery means resumption at a defined safe enclosing checkpoint, **not** return to the faulting instruction. A recover boundary must not violate borrow, lock, actor turn, or async continuation ownership invariants. If none exists, its `Raise` identity propagates through ordinary calls until the actor/non-actor task boundary converts it to terminal `Panic(origin=Raise)`.
3. Panic bypasses catch, trap, source recover, and **all actor-local unhandled hooks**. During actor unwind, any permitted cleanup is best effort and bounded by the actor termination budget; the terminal `Panic` must be published exactly once to the reserved parent/root CONTROL slot before terminal retirement when the hosting runtime is alive. The panicking actor cannot handle, acknowledge, replace, or consume its own panic.
4. After unwinding the source turn, an actor's uncaught `Throw` MAY enter its compiler-recognized `on_throw` hook; uncaught `Raise` MAY enter `on_raise`. These hooks run only on that actor's exclusive lane, after safe unwind; they cannot run concurrently with its own user code or retain dead lexical borrows. `Handled` terminates the failed turn and permits a subsequent turn only if actor state remains valid. **If absent, unhandled, or requesting escalation, the actor must terminate with `Panic(origin=Throw|Raise)` to its parent/root supervisor.** Hook failure may strengthen the failure, preserving the initiating origin as a cause/secondary diagnostic. There is **no actor-local `on_panic`**. The supervisor MUST NOT receive a naked terminal `Throw`/`Raise` from an actor.
5. A parent/supervisor receives a detached, bounded, immutable failure **envelope** via a pre-reserved dedicated CONTROL slot, not a guest exception on its call stack and not ordinary mailbox data. The supervisor's `on_panic` may choose stop/restart/escalate for the **child**; it never executes in the child's domain. Deduplicate by actor identity + generation; reserve capacity before spawn; handle CONTROL between single-flight actor turns. A supervisor itself failing is handled by its parent/root, never by recursively invoking its own callback.
6. No exceptional effect propagates synchronously *through* an actor-send or inter-isolate boundary. An actor task/future may settle with an immutable typed failure outcome; any supervisory routing remains separate. Cancellation is a distinct non-trappable control outcome.

## Non-actor execution, root task and root supervisor

Non-actor functions and ordinary nested calls use the **same lexical catch/trap/recover/unwind semantics**. A throw/raise travels through nested normal calls and async continuations while matching handlers remain available. **Only when it escapes the outermost non-actor task continuation** does it become terminal `Panic(origin=Throw|Raise)`. A direct panic is already terminal and cannot be caught or recovered. A non-actor task does not acquire an imaginary actor mailbox. Structured child tasks settle their task-group Future/outcome with the terminal panic and original provenance, so a supervising *non-actor main process* can inspect the task result, cancel siblings, or explicitly restart the task. A detached task must have a supervising owner or mandatory host failure sink; silent loss is forbidden.

The outermost non-actor `pub routine main()` runs under a root failure boundary owned by the host launcher/embedding runtime. The root is not its own supervisor. A **non-actor main running a supervisor** may consume child-actor terminal panic events on its reserved root CONTROL queue, select restart/stop/escalate, and remain alive when a child fails. A **non-actor main running ordinary structured child tasks** instead receives terminal task outcomes via the task group/Future, *not* actor CONTROL. Main's own unhandled throw/raise/panic becomes an unhandled root panic (preserving origin), and the launcher reports failure; neither a child supervisor handler nor main may silently consume its own root panic. External host/process manager restart policy is separate.

| Root outcome | Portable standalone CLI behavior | Embedded host behavior |
|---|---|---|
| Normal `main(): void` return | Exit 0 | Successful result |
| Normal `main(): int` 0..255 | Exit requested value | Successful result with status |
| Unhandled `throw` | Report root `Panic(origin=Throw)`; exit **1** | Return/complete terminal `Panic(origin=Throw)` |
| Unrecovered `raise` | Report root `Panic(origin=Raise)`; exit **1** | Return/complete terminal `Panic(origin=Raise)` |
| Unhandled `panic` | Report root `Panic(origin=Panic)`; exit **1** | Return/complete terminal `Panic(origin=Panic)` |
| Unhandled root cancellation / failed initialization | Exit **1** | Return/complete distinct failure |
| Explicit authorized `std.process.exit(n)` | Respect the existing 0..255 and first-authoritative-exit policy | Return explicit status, **do not call OS exit** |

Exit status 2 remains reserved for invalid CLI invocation; do not overload it to mean `raise`. The exit-status contract is `contracts/PROCESS_EXIT_STATUS_V1.md`. A failure that predates deferred `process.exit(0)` must never be converted into success. OS fatal signals and truly uncontainable native faults follow host/OS behavior; do not claim that an in-process root supervisor survives process death.

## Failure precedence and diagnostic integrity

Retain the shared host precedence lattice: `HostFatal/ProcessExit > Cancelled > Panic > Raise > Throw > Return`, with the **existing explicit-process-exit chronological exception** above. This ranking selects the primary failure when simultaneous failures coexist. The later actor/task/root *boundary normalization* of unhandled throw/raise into panic is a separate mandatory step. On equal priority retain the first failure as primary. Preserve later failures and bounded source breadcrumbs as secondary metadata. Do not carry raw Throwable/C++ exception pointers, mutable guest state, or arbitrary actor-owned objects across domains. Unknown effect tags, allocation failure on CONTROL delivery, stale generation, and exhaustion of failure slots must fail closed.

## October 12 hardening: panic-only actor CONTROL and retained cause codes

The actor-terminal CONTROL delivery boundary is **mandatory and idempotent**:
`Throw(code)` -> `Panic(origin=Throw, originCode=code)`;
`Raise(code)` -> `Panic(origin=Raise, originCode=code)`;
`Panic(code)` -> `Panic(origin=Panic, originCode=code)`.
Supervisor escalation must forward the existing terminal kind, origin kind and
origin code without reclassifying it or replacing it with a new generic panic.
The immutable diagnostic message is bounded; numeric codes are unsigned 32-bit.
No actor-owned error object, Java Throwable or C++ exception pointer may cross
CONTROL; structured diagnostics must be copied before actor reclamation.

The source actor may execute eligible `on_throw`/`on_raise` at most before
terminalization. The supervisor cannot receive a naked terminal `Throw` or
`Raise` even if an internal caller bypasses the higher-level bridge and
publishes directly to the CONTROL queue. Unknown tags, contradictory
terminal/origin pairs and invalid error-code ranges must fail closed without
consuming the reserved slot. Valid duplicate publications remain idempotent.
Cancellation and host-fatal control remain distinct from guest Panic.

The Java `ExceptionBoundaryPolicy` and native
`RootFailureBoundary.hpp` helpers encode the **same normalization rule** for
a non-actor task/root boundary but are currently pure policy helpers. Native
`EffectSupervisorBridge`, `SupervisorControlTurn`, and
`SupervisorFailureLane` now apply actor-boundary panic normalization;
this does **not** imply that Java `ActorRuntime`, Java
`LinkedProgramRunner`, or either source compiler has been wired to the
typed exceptional-effect ABI. No source parity claim is permitted until
end-to-end fixtures pass on identical source and binaries.

## Required source-level parity release gates

- Typed signature/effect inference: unannotated local inference, exported upper bounds, trait and function-value substitution, typed catch mismatch, dynamic/FFI unknown effect rejection.
- Actor source: matching throw catch, trap `None`, unmatched throw -> on_throw -> `Panic(origin=Throw)` CONTROL; raise -> eligible recover or on_raise -> `Panic(origin=Raise)` CONTROL; direct panic -> `Panic(origin=Panic)` CONTROL **without any on_panic/on_throw/on_raise/recover callback on the failing actor**. No supervisor sees terminal actor `Throw`/`Raise`.
- Non-actor source: nested synchronous, suspended asynchronous, detached task, root `main`, non-actor root supervisor child failure, supervisor self-failure, and explicit exit races. Check task/root boundary conversion from unhandled throw/raise to `Panic` with exact origin; assert status **1** for all unhandled root panic outcomes without inventing origin-specific exit codes.
- Cleanup: defer/finally LIFO through every channel, throws during panic, stronger cleanup escalation, invalid recovery, pending failure vs process.exit(0), actor teardown budget, and all suppression ordering.
- CONTROL: slot reservation before spawn, full normal mailbox, one event per generation, stale and duplicate publication, parent termination, root escalation, handler panic, bounded diagnostics and no actor self-interception.
- AOT/Graal/native LLVM/Wasm as applicable must use the **identical Oreslang fixture blob** and prove behavior after compilation. Host policy unit tests, reserved lexer tokens, and fail-closed fixtures alone do not establish executable guest parity.

**Compatibility note:** older proposals describing actor panic as recoverable or delivering unhandled actor throw/raise directly to CONTROL are superseded: unhandled throw/raise is converted to terminal panic at the actor/task/root boundary and the origin is preserved. There is never an intermediate implicit throw -> raise conversion. The intended release gate is an executable, two-backend, source-level proof; until then, reject unsupported syntax.
