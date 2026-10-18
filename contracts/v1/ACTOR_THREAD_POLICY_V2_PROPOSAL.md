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
   with an atomic bounded counter and **unique per-execution-segment grant
   nonces**. A ticket is bound to actor identity, generation, grant nonce,
   task/continuation and native carrier. Carrier identity alone is never a
   grant identity: after release/await, the same worker may obtain a fresh
   ticket, and a delayed release from its previous turn must be rejected.
   Exhausting the nonce space fails closed rather than wrapping (ABA).
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

## Audited v2 ticket model and v1 admission boundary (2026-10-17)

The finite `scripts/verify_actor_thread_policy_v2.py` model now explores
atomic grants as `(actor_generation, segment_nonce, carrier_owner)` and
models retirement before incarnation restart. It checks caps 1/2/3, unique
carrier owners, distinct tickets, stop fencing and stale-generation denial.
A regression specifically releases one ticket, reclaims the *same* carrier
with a new ticket, then attempts to release the earlier ticket: the fresh
grant must survive. Cross-carrier releases and partial shutdown are denied.

Java `ActorThreadAdmission` is independently called by the type-checker,
compiler API and Truffle guest-execution boundary. JVM host adapter
annotations must be validated before registration; reflection alone grants
no scheduler authority. LLVM code generation separately rejects forged
`Program` objects whose actor limits are not (1,1), even if source parsing
was bypassed.

These are **v1 fail-closed controls** and abstract v2 protocol evidence.
They do not certify lock-free grant implementation, actor-field races,
reentrant critical callbacks, same-actor true parallelism, foreign
callbacks, supervisor CONTROL or cross-backend scheduler parity.

## Hardened implementation evidence (2026-10-17 follow-up)

The Java/GraalVM branch now independently checks actor policy at direct AST
compiler entrypoints, diagnostic analysis, the type-checker/annotation-expansion
boundary, direct tree-shaking, and the Truffle execution boundary. AST
reconstructors preserve the policy instead of recreating a default (1,1)
policy. A host-only `ActorExecutionGrantGate` demonstrates synchronized,
bounded physical claims, unique single-use carrier tickets, incarnation
fencing, stop/seal and retirement, with 12-way contention and stale/foreign
release JUnit fixtures. This host test primitive is **not attached to production
actor dispatch** and does not authorize `max > 1` source execution.

The LLVM branch rejects forged `ActorDecl` policies during IR generation,
not merely in its parser, and adds direct Program-to-LLVM rejection tests,
executable source fixtures and no-IR rejection cases.

Both branches run the same abstract Python grant model when their CI runners
are available. Independently executing the model locally produced:
`cap=1: 104`, `cap=2: 744`, and `cap=3: 2184` reachable finite states;
a disabled capacity guard produced a four-claim counterexample. The model also
checks same-carrier release/reacquire ABA, cross-carrier release, duplicate and
stale tickets, seal/retirement, and generation transitions. These are
**abstract bounded-model results only**, not evidence of JDK25, native-image,
LLVM/CMake, or concrete actor scheduler correctness.

Production v2 remains blocked on verified actor effect checking, all callback
and FFI entrypoints, scheduler ownership, heap-state parallelism, logical task
and state-loan retirement, supervisor CONTROL priority, and exact-tree green
JVM/native CI. In particular, a zero-step GitHub Actions failure is a CI
infrastructure failure/unknown result rather than an executable test verdict.

## Host prototype hardening: carrier non-nesting and physical-only teardown

The host-only `ActorExecutionGrantGate` must prevent the same platform
`Thread` from holding execution grants for two different actors concurrently,
even if each actor is below its local maximum. Cross-actor synchronous nested
guest execution is **not** a permitted replacement for message dispatch.
Thread-local carrier ownership must be cleared by the scheduler in a
`finally` when the executing segment exits. Because the host prototype
identifies physical ownership via `Thread.currentThread()`, it fails closed
on Java **virtual threads** rather than incorrectly treating multiple virtual
threads on one carrier as separate physical workers. A production integration
may instead use a proven platform-carrier identity abstraction.

A sealed physical gate may expose an interruptible, timeout-bounded
`awaitPhysicalDrain` that waits for active physical grants to leave. It
must reject waits from a thread already holding an actor execution grant,
reject waits before `seal`, and never reopen admission on interruption.
`tryRetirePhysicalGate` marks only the physical execution gate as retired.
It **must not** be interpreted as actor completion: queued messages, suspended
logical tasks, state loans, CONTROL callbacks, supervision and heap cleanup
have independent retirement barriers.

The host-side prototype and its JUnit tests are not wired into production
`ActorRuntime`. Its process-wide `ThreadLocal` is a test/development
prototype and is not the final VM-wide carrier identity mechanism, including
for classloader isolation and non-JVM backends. The scheduler must never hand
guest code raw `Grant` objects. Under guest panic/raise/throw, cancellation
and forced unwind, permits are returned exactly once by a trusted `finally`
boundary. If a worker dies before returning its ticket, recovery must fence
the actor incarnation without reusing a still-live ticket identity.

The shared finite model additionally explores a two-actor/four-carrier state
space with actor capacities 2 and 3 and proves the same carrier cannot hold
two actor grants in a valid state; disabling the global exclusion guard
produces a two-claim counterexample. This is a bounded abstract check, not
end-to-end scheduler or effect-checking evidence.

## Canonical configuration and compatibility consolidation (2026-10-17)

The **single authoritative AST/ABI description** is
`ActorThreadPolicy(min, max)`, encoded as two positive bounded integer
fields, with `1 <= min <= max <= 3`. Annotations are declarative inputs;
neither of these fields is an executable scheduler grant.

The new primary spellings are:

- `with threads(min: 1, max: 3)` in a source actor header;
- `@ActorThreads(min = 1, max = 3)` on a source actor declaration.

Compatibility for the earlier MaxActorThreads proposal is explicit:

- `@ActorMaxThreads(MaxActorThreads.LOW)` is accepted and **normalizes
  immediately** to `ActorThreadPolicy(1, 1)`; the enum itself is not a
  separate execution-policy field.
- `MaxActorThreads.MEDIUM` and `MaxActorThreads.HIGH` are recognized but
  fail closed until actual parallel guest execution is certified, exactly
  like `@ActorThreads(max = 2/3)`.
- The exact historical `define enum MaxActorThreads as LOW int = 1;
  MEDIUM int = 2; HIGH int = 3 end` declaration is a narrowly recognized
  frozen compatibility form, **not** general user-defined enum lowering.
  Reordered values, redefinitions and duplicate declarations are errors.
- Mixing `@ActorMaxThreads`, `@ActorThreads` and the native `with threads`
  clause, even when all specify (1,1), is **a compile error**. A single actor
  must never have two policy sources.
- Old Java AST consumers may use `ClassDecl.maxActorThreads()` or
  construct an AST from the legacy enum only for policies with `min=1`.
  If `min>1`, this compatibility getter now **throws** rather than
  silently erasing the requested floor during a legacy AST round trip.
  New compiler passes must carry the whole `actorThreads` record.

Current version scope is bounded to three slots. Future >3 behavior requires a
new versioned ABI and scheduler/effect-checking qualification; it is not
implicitly enabled by a widened host-side counter or manually fabricated AST.

### Integration boundaries

Java #1100 and LLVM #485 contain earlier host-only NIO/turn-credit designs
and still require reconciliation with the new canonical policy. **Do not**
cherry-pick their parser/AST code over Java #1182 or LLVM #541 without
conflict-resolving this schema; that would silently resurrect two independent
policies or lose one during AST rewriting. They are not validated as a
production multi-turn runtime.

The production v1 scheduler is still single-carrier. The host-side grant gate
in Java #1182 is a separate prototype and cannot substitute for scheduler
admission, backend ownership checking, actor state-loan verification, or
per-turn continuation frames. Any transition to actual max=2/3 guest
execution requires explicit feature negotiation and all v2 runtime tests.

### Source and CI provenance

Both GitHub source organizations currently report workflow failures with
zero executed job steps on these PR heads: this is **NOT_EXECUTED**, not a
compiler pass/failure. Only source-local JDK25/Graal Native Image and
LLVM/CTest/ASan/UBSan execution on the exact Git tree can qualify the release,
or an independently authorized private execution environment that has passed
the mandatory two-repository numeric-ID/visibility preflight. Source-free
models and focused external component runs are separately labeled and cannot
be promoted to full-tree runtime evidence. No alternate-organization transfer
is authorized by this proposal.

### Adversarial integer and compatibility hardening

Actor `threads(min: ..., max: ...)` and `@ActorThreads(...)` values
require positive ASCII decimal integers. Underscores, when present, must
separate two digits; a trailing, doubled or isolated underscore is invalid.
This rejects otherwise-ambiguous capacity literals such as `1_` or `1__`
without changing the general-purpose Oreslang numeric lexer. Java and LLVM
share this rule and have positive/negative source regression cases. The
legacy `MaxActorThreads` enum backing integers are canonical exact tokens
without separators.

In all backends, **physical actor execution remains max=1** until the real
scheduler admission, ownership/effect checking and full-tree tests run.
Host gates and syntax-only compiler tests cannot authorize parallel actors.
