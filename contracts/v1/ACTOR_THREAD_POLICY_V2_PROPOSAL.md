# Actor execution thread policy — proposed v2 transition

**Status (2026-10-17): proposal and guarded front-end support, NOT a parallel-execution implementation.**
This contract is intentionally separate from `ACTOR_EXECUTION_EXCLUSIVITY_V1.md`.
The v1 exclusive-carrier invariant stays normative for production runtimes
until a versioned v2 backend has passed all the gates below.

## Language spelling and Java interop

```ores
define actor Worker from private actor with threads(min: 1, max: 3) as
    receive(ActorMail<Job> mail) {
        process(mail.value);
        mail.done();
    }
end
```

Equivalent declarative source annotation:

```ores
@ActorThreads(min = 1, max = 3)
define actor Worker from private actor as
    receive(ActorMail<Job> mail) {
        process(mail.value);
        mail.done();
    }
end
```

On Java hosts the same metadata may be declared via
`dev.oreslang.runtime.ActorThreads` with `@Target(TYPE)` and
`@Retention(RUNTIME)`. **Java reflection of an annotation does not grant
execution authority**: bridge registration must consume a validated descriptor
and refuse backends that cannot honor it. Native-image may process metadata at
build time; no runtime reflection is required by the language semantics.

Both spellings normalize to `ActorThreadPolicy(min,max)`.
Defaults are `(1,1)`, with `1 <= min <= max`. Values are compile-time
positive integers. Duplicate keys, unknown keys, duplicate annotations, mixing
both forms on the same actor, and annotations on non-actors are errors.

`min` is a requested execution-capacity floor **under load**, not a reservation
of dedicated operating-system threads and not permission to exceed the global
scheduler budget. An implementation may reject an unrealizable configuration,
rather than claim its requested minimum is guaranteed. Worker carriers remain
shared and may change after a suspension.

## Separate concurrency axes

- `requestConcurrency`: outstanding request lifetimes;
- `messageConcurrency`: outstanding message lifetimes;
- `threads.max`: simultaneously executing actor-owned segments.

A suspended handler does not occupy a physical slot. At an instant, at most
`max` distinct carrier owners hold valid execution grants for one actor
incarnation. A carrier can count once, even if nested same-carrier calls occur.
The grant cannot be forged, borrowed across actors, retained across awaits,
or transferred to a different carrier without return to scheduler admission.

## Required v2 safety model

1. Every actor incarnation owns one generation-stamped admission authority
   with an atomic bounded counter and distinct owner tokens. A ticket is bound
   to actor identity, generation, task/continuation and native carrier.
2. A valid ticket is acquired before entering *any* actor guest code, including
   linked calls, callbacks, timers, FFI, supervisor CONTROL, lifecycle hooks and
   finalizers. Wakeups and foreign threads only enqueue work.
3. Regular actor mutable fields may not be accessed concurrently without
   compiler-proven ownership partitioning, immutable snapshots, approved
   atomics or a serialized `critical handle_state` transition. The compiler
   must reject unsafe code, not infer safety from an annotation.
4. A state loan, whether mutable or read-only, cannot outlive its verified
   scope or cross `await` unless a separate explicit checked contract exists.
   A critical callback cannot be reentered by the same actor through another
   carrier unless the critical protocol explicitly allows it.
5. A source mailbox has FIFO *admission* sequence; completion order may vary.
   Output sequence numbers are assigned on successful emission and never reused.
   Consumers must not rely on parallel handlers completing in admission order.
6. Graceful stop seals admission, drains already admitted work according to
   lifecycle policy, and runs final cleanup exactly once **after** all execution
   grants, logical continuations and state loans have quiesced. Cancellation,
   errors and force-termination must not free memory while a grant is live.
7. Supervisor CONTROL is privileged and cannot be starved by unlimited DATA
   messages. CONTROL may have a reserved queue but no privileged bypass of
   actor state protection or execution limits.
8. `serial actor` remains max 1 and retains logical message admission
   across suspension. Its execution policy cannot be overridden by annotation.
   Existing code without explicit parallel policy retains v1 semantics.
9. SHARED, PRIVATE and UNTRUSTED actors preserve their capability policies.
   Same-actor intra-heap access by multiple carriers must be protected;
   actor-to-actor heap isolation is unchanged. Foreign-thread FFI cannot call
   into unadmitted actor contexts.
10. AOT/LLVM/.NET/WASM/Forge targets must either implement identical safety
    semantics or reject `max > 1` at compile/link/registration time. Silently
    falling back to max 1 is a behavioral mismatch.

## Release/admission gates

Stage A (current Java implementation): parse, validate and preserve metadata
for default/max-1 configurations; reject max>1 with a clear diagnostic. Java
host annotation type exists; generic host actor bridge remains a separate task.
Stage B: add an explicit execution-policy descriptor to both compilers/ABIs,
with backend capability negotiation and rejection on unsupported targets.
Stage C: prove concurrency grants (single winner default; 3-way cap under
racing claims), suspension/reacquisition, fairness, retirement and stale-ticket
denial using model checking and stress tests.
Stage D: effect/ownership verification for all actor entrypoints, including
FFI, callbacks, critical loans, supervisor and lifecycle barriers.
Stage E: run exact-tree Java/JDK25/native-image and native ASan/UBSan/CTest
conformance suites across targets. Only then may `max > 1` be admitted.

**Do not** edit v1 exclusivity proofs to falsely imply parallel runtime support.
