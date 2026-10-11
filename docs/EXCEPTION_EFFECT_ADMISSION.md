# Typed exceptional effects: logical invocation unwind and supervisor CONTROL

**Status (2026-10-11): design/admission contract, not executable source-language parity.** Both production frontends reserve `throw`, `raise`, `panic` and `recover`, but source-level compiler lowering, cross-await unwind frames, and supervisor CONTROL handlers remain separate release gates. Java/GraalVM already supports related `defer`, `try/catch/finally` and `trap` constructs; LLVM also has a native host exception *policy* helper. Token reservation or host tests must not be presented as guest-language execution.

Companion: [supervisor recovery contexts and nested actor families](SUPERVISOR_RECOVERY_AND_NESTED_ACTOR_FAMILIES.md).

## Effects and recovery

| Effect | Local typed catch/trap | Direct deferred `recover()` | After unhandled failure |
| --- | --- | --- | --- |
| `throw e` | Yes, matching handler | Yes, same logical invocation | Local `on_throw` / terminal resolution |
| `raise e` | No; explicit escalation | Yes, same logical invocation only | Local `on_raise` / terminal resolution |
| `panic e` | No | **Never** | Typed terminal CONTROL to supervisor |
| cancellation | No | **Never** | Cleanup and structured cancellation |
| process exit / host fatal | No | **Never** | Runtime/root process policy |

`recover()` is valid ONLY when invoked directly inside the currently executing deferred function *during* an active recoverable unwind. It returns the recoverable failure and consumes it exactly once; otherwise it returns `nil`. Calling a helper that calls recover is not equivalent to a direct deferred call. Cleanup defers run LIFO with bounded cleanup and suppressed-diagnostic preservation under the shared failure-priority lattice: host/process fatal > cancelled > panic > raise > throw.

`recover()` never rewinds execution after the failing statement or failed `await`. The recovered frame completes normal unwinding. For non-`void` return types the compiler must prove a valid value on the recovered-exit path, or reject the function; no implicit fabricated default value.

## ExecutionContext ownership

Every admitted actor-message invocation and every supervisor callback gets a *distinct* logical ExecutionContext with stack of frame-scoped defers, pending recoverable failure, structured cancellation and scheduler continuation metadata. Ordinary nested calls and **awaited** async functions share a logical unwind chain, even if the native carrier/thread changes. ThreadLocal/thread_local may cache the context pointer for the active turn, but cannot be the durable owner of recover state.

Detached tasks, other actor messages, independent child actors and supervisor callbacks DO NOT share recovery stacks. An explicit awaited result may deliver a serialized failure value, from which the awaiter can create a **new local throw**, but that is not cross-actor recovery.

## Supervisors

A child terminal panic becomes one immutable typed CONTROL event for its owning supervisor (not a direct function call from the child carrier). Supervisor `on_panic` runs with a NEW supervisor-owned ExecutionContext on the supervisor's exclusive single-flight lane. The callback may handle its own ordinary throws via `defer/recover`, but cannot consume the child's original panic. Supervisor panic escalates via CONTROL to the parent supervisor after cleanup. If root panics, runtime/root policy applies. Reserved CONTROL capacity must not preempt or reenter an in-flight user-code turn.

## Tests needed before source admission

1. Multiple `await` suspensions with carrier migration preserve LIFO defers and one direct recovery.
2. Ineligible indirect calls, detached tasks, sibling invocations and cross-actor panic cannot recover.
3. `raise` bypasses ordinary catch but may be consumed by direct defer; `panic` never can.
4. Supervisor handler context remains isolated; true supervisor panic escalates.
5. Cancellation runs cleanup and is not recoverable; cleanup-failure priority/suppressed metadata is stable.
6. Non-void recovery paths need definite typed returns.
7. Test exact guest source on Java/GraalVM and LLVM/Clang, with matching traced semantics and identical source Git blobs; no claims of parity from lexer or host scaffolding alone.

First executable milestone: a `void` actor invocation with `defer`, `throw`, and direct `recover()`, plus a non-recoverable panic that results in an immutable supervisor CONTROL notification.
