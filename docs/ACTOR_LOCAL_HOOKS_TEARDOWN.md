# Actor-local unhandled hooks and deterministic teardown (proposed parity contract)

Status: **language contract and incremental host/runtime prototype, NOT source-level
compiler completion or release certification.** This document applies to both
`oreslang-source.java` (Graal) and `oreslang-source.llvm`.

## Two distinct hook domains

The syntax `define actor M ... @Implements on_throw/on_raise ...` in this RFC
refers to the actor's **own** uncaught exceptional effect, not its children's
failures. Parent/supervisor CONTROL handlers are distinct. Existing Graal
prototype source hooks named `on_throw/on_raise/on_panic` currently mean
**child-supervision**; this is a compatibility conflict. Do not silently
reinterpret those existing hooks or accidentally let an actor catch its panic.
Proposed final source names:

| Invoked on failing actor's own exclusive logical lane | Invoked on parent's exclusive CONTROL lane |
| --- | --- |
| `on_throw(UncaughtThrow<T>)` | `on_child_throw(ActorFailure<T>)` |
| `on_raise(UnrecoveredRaise<T>)` | `on_child_raise(ActorFailure<T>)` |
| `tear_down(TeardownContext)` | `on_child_panic(ActorFailure<PanicInfo>)` |
| **NO `on_panic` self handler** | `on_child_cancel`, `on_child_host_failure` |

An inherited compatibility alias for legacy parent `on_panic` may be supported
temporarily only if statically resolved to the **parent** notification ABI.
No guest-defined local panic handler and no reflection/direct invocation of
these hooks through `ActorRef`; actor communication remains `send(T)` only.

## Uncaught failure semantics

- `throw E` searches typed `try/catch`. If no catch handles it after
  lexical `defer/finally` unwind, offer a typed immutable summary to the
  **same actor's** `on_throw` exactly once.
- `raise E` searches matching explicit `recover` scopes (not catch/trap).
  If unrecovered after unwind, offer it to `on_raise` exactly once.
- The hook returns `FailureAction.handled` or `FailureAction.escalate`
  (a no-return shorthand must NOT silently swallow exceptions). `handled`
  means the current receive/continuation failed irreversibly but future mail
  can proceed after compiler-verified state invariants and loan release.
  It does NOT resume the failed instruction, replay a consumed message,
  or ignore uncommitted outbound sends.
- A missing hook, its own failure, an invalid state invariant, or explicit
  `escalate` triggers fail-stop and a typed parent CONTROL event. Hooks are
  non-recursive; a throw/raise in the hook must not re-enter the same hook.
- `panic` bypasses `catch`, `trap`, `recover`, local failure hooks and
  any guest-level `on_panic`. The runtime stops the actor, preserves the
  PANIC effect, and publishes to the parent's reserved CONTROL lane.
  **Only the supervisor can decide** stop/restart/escalation.
  Native/VM fatal failures are not guaranteed to be containable.
- Cancellation is control, not throw/raise. Neither local hook can intercept it.
  A parent scheduler/supervisor cannot invoke a child's hooks directly.
- Exception callbacks and `tear_down` are scheduler-admitted turns, NEVER
  concurrent with `receive`, `handle_state`, or another continuation. Nested
  state-loan and `await` safety rules still apply.

Suggested guest syntax (not yet accepted by both compilers):

```ores
define actor M<messages=Messages> as
    @Implements
    on_throw(UncaughtThrow<Error> e): FailureAction {
        return FailureAction.handled;
    }
    @Implements
    on_raise(UnrecoveredRaise<Error> e): FailureAction {
        return FailureAction.escalate;
    }
    @Implements
    async tear_down(TeardownContext ctx): void {
        await ctx.connections.close_gracefully();
    }
    @Implements
    receive(Message m): void { }
end
```

## Teardown and memory ownership

The actor lifecycle is monotonic:

```
RUNNING → QUIESCING → TEARING_DOWN → RETIRING → FINALIZED
```

1. Seal new DATA admissions and atomically revoke outgoing capabilities;
   decide policy for already queued messages (reject/drain with accounted
   release, never execute new guest receives after seal).
2. Stop or cancel descendants under structured ownership. Cancel pending
   timers, channel/select waiters and async/FFI continuation tickets. Await
   currently admitted turn to relinquish the exclusive logical lane.
3. On **graceful** stop only, run `tear_down` at most once as a special
   lifecycle CONTROL turn with restricted access to still-live resources.
   Permit bounded asynchronous close/flush via nonblocking await; yielding
   relinquishes the lane and resumption must reacquire a scheduler ticket.
   No new actor spawns, mailbox reopens, state-loan escapes, or arbitrary
   unbounded resource allocations during teardown.
4. Enforce a supervisor-configured cleanup deadline/CPU/fuel budget. A cleanup
   throw is recorded as a secondary failure or becomes the final failure for
   otherwise-normal stop. Deadline expiration switches to forced retirement.
5. **PANIC is special:** publish the terminal PANIC to the supervisor without
   invoking the failed actor's guest `tear_down`. Trusted runtime finalizers
   still close/fence handles when safe. A supervisor may orchestrate external
   graceful disconnection, but may not allow the panicked actor to trap panic.
6. Once all active turns, FFI callbacks, native loans, and pending CONTROL
   reservations are quiescent (or safely revoked and fenced), close the actor's
   persistent arena/region, invalidate generation-tagged handles, release
   scratch regions, mailbox/output envelopes, connection/FD registrations,
   and owned subgraphs; clear the last strong owner references.
   `get_done()` resolves only after authoritative finalization.
7. Each resource close is idempotent. Cleanup uses a bounded per-actor
   registry and reverse acquisition order, with independently attempted
   closes and preserved suppressed failures. Exactly-once logical close
   does not imply physical OS close is always immediately possible.

`handle_state` is the exclusive state-access callback. State must not escape
through `tear_down`, self hooks, user mail, native FFI or async captures; the
compiler must check ownership, generation, loan and alias boundaries. Neither
ordinary Java GC nor OS-process memory reclamation should be mislabeled as
per-actor deterministic reclamation: native FFM/LLVM arenas can be explicitly
retired after quiescence, while Graal Java heap objects are only guaranteed
*eligible* for GC once no references remain. A physical crash or SIGKILL
cannot execute a guest graceful hook. OS-process isolation is required for
strict fatal-memory fault containment.

## Acceptance tests (all required for release)

1. Host and guest `throw`/unrecovered `raise` callbacks each fire once on
   the failing actor's exclusive turn, handle or escalate explicitly; a handled
   fault allows subsequent messages but never resumes the failed statement.
2. Panic inside receive, initializer, await continuation, or cleanup never
   calls an actor-local failure hook; the supervisor receives one typed PANIC,
   even if DATA mailbox is full.
3. Synchronous, async, concurrent external stop and cancellation invoke
   graceful `tear_down` at most once where permitted; active `handle_state`
   and cleanup never overlap; timeout forces bounded retirement.
4. Close connections/streams/timers/FDs and FFI resources; zero outstanding
   owned reservations and loans before actor DONE, including cleanup errors.
5. Synthetic native heap and source guest state handles fail on generation
   reuse; parent/root remain alive, no stale continuation resurrects an actor.
6. Both LLVM and Graal run byte-identical guest fixtures with the same
   THROW/RAISE/PANIC/control ordering. Run full source compiler, native/AOT,
   sanitizers, fault injection, and funded exact-head CI before marking ready.

## Prototype coverage

- The Java/Graal `ActorRuntime.LocalUncaughtFailureHandler` host behavior
  protocol is the first bounded execution prototype: only typed guest
  THROW/RAISE escaping a host behavior's message/continuation can be locally
  handled, panic bypasses, and current receive is never restarted.
  Source-level `@Implements` lowering is **not** yet implemented.
- Existing Graal parent `SupervisionHandler`, guest `on_throw/on_raise/on_panic`
  source hooks and reserved ancestor CONTROL events currently refer to
  **children**, not this new self-local contract. Renaming/migration is gated
  on parser, type checker, evaluator and cross-backend conformance.
- Existing Java FFM and LLVM persistent actor heaps have host-level
  quiescent-release primitives, but guest `tear_down` turns and complete
  source-level transitive memory/connection ownership remain unimplemented.
- LLVM still fails closed for unsupported `throw/raise/panic/recover`
  source grammar; a native-only routing-policy test does not establish
  guest-source lowering.
