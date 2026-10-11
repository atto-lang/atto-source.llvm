# Implicit scheduler and mailman; optional policy customization (proposal)

**Status (2026-10-11): design proposal only. No `define scheduler` or `define mailman` source-level parser or guest lowering is claimed.** This file does not authorize changing the runtime's exclusive actor-turn scheduler or host CONTROL authority. Companion: `SUPERVISOR_RECOVERY_AND_NESTED_ACTOR_FAMILIES.md`.

## v1 defaults — no user declaration

A source program can use `define actor`, `define supervisor`, `spawn`, `ActorRef.send(typedMessage)`, and `await` without defining any scheduler or mailman. Each actor/supervisor executes one logical user-code turn at a time; distinct actors may execute concurrently on bounded, actor-kind-scoped shared physical carrier pools. Mailboxes, backpressure, typed serialization, and reserved CONTROL dispatch are runtime-owned.

A -> B -> C nested spawning does NOT create another group, supervisor, scheduler, or mailman. The family inherits the same *managed lifecycle ActorGroup* from its root when that root has been enrolled. Distinct actors retain distinct single-flight execution lanes and independent defer/recover execution contexts. Existing spawn-lineage cancellation remains.

An `ActorGroup` is **not** an OS thread pool or a single dispatcher: it is a coordination/event domain with an explicitly managed worker-lifecycle registry. Its default mailman is the runtime-controlled bounded event/message delivery path. Group ownership must not imply event-bus membership or delivery capability.

## Why customizable scheduler/mailman source forms are future features

Potential syntax, reserved for design and **not implemented**:

```ores
define scheduler WeightedFair as
    // Declarative scheduling policy only, not a replacement run loop.
end

define mailman PrioritizedDelivery as
    // Declarative delivery policy only, not a replacement transport.
end
```

For v1, users configure **existing runtime scheduling and mailbox policy** through bounded, supervisor/group-level configuration, not arbitrary user-code callbacks. Future custom source policy declarations should be restricted to safe, total, bounded functions over immutable metadata (turn readiness, queue pressure, message priority, quota) or declarative config. Do not allow arbitrary I/O, native FFI, `await`, actor state loans or blocking inside scheduler/mailman admission callbacks. Any callback must run within a budget, fail closed on errors, and never gain host CONTROL authority.

### Non-negotiable invariants

- No custom scheduler can run two turns of the same actor/supervisor at once, resume a suspended callback without its admission fence, starve reserved CONTROL, bypass cancellation, or grant actor-owned-memory access.
- No custom mailman can invoke actor methods directly; cross-actor communication is only typed `ActorRef.send(payload)`. Sender admission validates serialization, ownership transfer, isolation, queue quotas and backpressure, before the delivery policy runs.
- No user-defined mailman intercepts or drops terminal panic/control-plane messages; supervisor failure notices use a trusted reserved CONTROL path, separate from ordinary user mail.
- No mailman routes an actor failure to its spawning actor simply because the actor was spawned there. Supervisor of A/B/C remains the explicitly owning S by default.
- Scheduling and delivery policies never own a `recover()` stack. Durable invocation ExecutionContext follows logical nested calls/`await` and not carrier threads, policy callbacks or ActorGroup identities.
- Group and child shutdown work after terminal failure must still run bounded cleanup and wait for actual physical finalization before `get_done()` resolves.
- The runtime must provide sensible defaults, and its API should offer configuration before inventing a custom scheduler DSL. Implement source policy syntax only after testable requirements arise.

## Future extension acceptance gates

1. Typed, immutable policy-input ABI and validated group-level configuration with fail-closed overflow/error handling.
2. Proven starvation bound, deadline fairness and CONTROL priority under load.
3. No policy can block a carrier or run guest code on an unadmitted completion thread.
4. Security tests: untrusted policies cannot forge actors, refs, group capabilities or supervisor CONTROL.
5. Exact source-level GraalVM + LLVM parity proof when the syntax is actually implemented.

The design decision is **implicit by default; optional safe policy specialization later**.
