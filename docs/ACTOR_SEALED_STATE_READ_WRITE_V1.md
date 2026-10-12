# Actor sealed state and read/write critical admission v1

Status (2026-10-12): **contract plus incremental lexer/negative-check and native
logical ticket substrate; not Java/LLVM source-codegen parity**.

## Source contract

Actor `state { ... }` is a **single compiler-owned declaration**, not an
ordinary field. Never materialize it as a guest-visible `self.state`, reflective
field, dynamic property, native pointer, inherited field, or mutable alias.
Only the sealed, non-overridable `self.handle_state` intrinsic may mint an
ephemeral state capability. A class/actor cannot supply its own implementation
of `handle_state`. Type-check/validate all field accesses, including through
aliases, closures, getters, cast/dynamic/reflection and FFI.

Proposed canonical source (positive only after complete backend lowering):

```ores
define actor Foo as
    state { v: int = 0; }

    pub increment(): int using state.read, state.write {
        return self.handle_state(critical-write |s| -> {
            s.v = s.v + 1;
            return s.v;
        });
    }

    pub get(): int using state.read {
        return self.handle_state(critical-read |s| -> s.v);
    }

    receive(Message m): void {
        // self.state.v = 3;    // error: self.state is not in scope
        // val i = self.state.v; // error: self.state is not in scope
    }
end
```

The `handle_state` argument must be a lexically inline callback taking
**exactly one** state-access parameter. Unmarked callbacks remain forbidden:
`self.handle_state(|s| -> ...)` is not a synonym for a read, since mutation
and effect inference have to agree. Preexisting `critical |s| -> ...` remains
the exclusive mode for backward compatibility. `critical-write` is its
explicit new spelling; `critical-read` is a read-only mode. A read-only
callback must not call an unknown/mutable function, return/store/borrow its
state capability, or retain a state-backed alias through suspension. A write
callback may read and mutate; `using` must cover both actual effects.
A direct assignment without reading its prior value requires state.write only.

`critical` on file/static/instance callables retains existing exclusive
logical-gate behavior. Proposed `critical-read` on a callable uses the shared
read gate and `critical-write` uses the exclusive writer gate, scoped to the
same instance/code-unit as the existing critical member gate. The parser may
eventually accept modifiers before or after `fnc`; **do not** accept them as
no-op modifiers before compiled metadata and scheduler handling exist.

## Keyword tokenization

The hyphen is ASCII U+002D (`-`, HYPHEN-MINUS).
Recognize `critical-read`, `critical-write`, `transform-pipe`, `nb-select`
only as exact contiguous reserved spellings. Ordinary `a-b` and `a - b`
remain subtraction; `a->b` remains the arrow. No generalized hyphenated
identifiers, and do not combine tokens across spaces/comments. Unknown forms
fail closed; `transform-pipe` and `nb-select` are reserved without asserting
runtime syntax support.

## Actor execution, snapshots and fairness

Oreslang's **at most one physical guest executor per actor** invariant is
unchanged. Multiple logical read permits must **not** cause the same actor's
guest code to execute on multiple carriers. Shared reads can run in parallel
only on verified immutable, generation-pinned snapshots outside that actor's
single-flight guest turn; ordinary in-actor callbacks still execute serially.
Without snapshot lowering, `critical-read` must fail closed rather than
claim parallelism. A writer is exclusive against readers and writers, never
held as an OS mutex across a guest callback/await.

Admission is FIFO with consecutive readers in one cohort, bounded queue,
cancellation-aware tickets, no barging after a queued writer, and a scheduler
CONTROL wake path. Tokens are gate scoped, monotonic and generation-checked;
retirement revokes tickets. No implicit read-to-write lock upgrade (release
and retry with an application-level version check). Same-continuation nested
callbacks must fence the outer loan and use trusted reentrancy handling; a
new ticket must not deadlock its own parent. A yielding callback retires its
state loan before yielding, reacquires a fresh ticket/loan after resumption,
and does **not** promise callback-wide atomicity across await.

## Release gates

- Reject direct read, write, address-taking, aliasing, reflection, state
  shadowing or dynamic lookup of `self.state`; reject overriding handle_state.
- Reject non-inline callback, wrong arity, escaping loan, understated effect
  row, writes in read mode, unknown transitive FFI, and live loan at await.
- Verify token writer fairness, reader cohort, cancel/handoff race, stale/
  duplicate/foreign tickets, shutdown and actor exclusivity.
- Lower runtime-owned typed state for Java/Graal and LLVM before accepting
  positive source. Run identical positive/negative source fixtures on both
  backends, with sanitizer and parallel stress tests. Existing lexer/ABI
  tests alone cannot satisfy this release gate.

See `CRITICAL_HANDLE_STATE_USING_PARITY.md` and
`CRITICAL_CALLABLE_SEMANTICS.md` in the LLVM repository for prior contracts.


## Hardening audit (2026-10-12)

The first implementation found and addressed the following boundary gaps:

1. Java parsing denies direct `self.state` within actor classes and rejects
   actor-defined `handle_state` methods (including `static fnc` declarations).
   Java's type checker additionally rejects `.state` access based on a
   receiver's nominal actor type; aliases cannot bypass the literal syntax
   check. These checks do not imply the positive state schema is lowered.
2. The native RW admission substrate caps **active reader tickets at 1024**
   and queued tickets at **8192**. Writers are FIFO-ordered; arriving readers
   cannot overtake an already queued writer. Modes outside Read/Write reject
   before any ticket is minted.
3. C++ admission transfers allocated linked-list nodes from waiting to active
   ownership, without further allocation during handoff. Before changing
   ownership the release method reserves all needed wake-list space, so
   an allocation failure cannot strand a newly admitted ticket. Linear
   active-ticket lookup is the deliberate safety tradeoff pending profiling.
4. Retirement returns **all outstanding owner/waiter tokens** for terminal
   cancellation instead of silently dropping waiting continuations. Runtime
   scheduler integration MUST consume this batch and signal cancellations
   outside the gate metadata mutex. This adapter is not integrated yet.
5. Multithread cancellation/handoff, queue ordering, retirement and active
   reader bounds have targeted C++ tests. CI jobs currently fail before steps
   execute; those tests remain unverified on a build runner.

**Must remain draft:** no source-level critical-read/write effect or loan
lowering, no trusted immutable snapshot proof, no scheduler ticket binding
to guest continuations, no user-visible actor-state read concurrency, and
no demonstrated Java/LLVM end-to-end parity. A successful native gate test
is not evidence that guest actor execution is race-safe. Explicit read-to-write
upgrades are forbidden pending versioned retry semantics.


## Native continuation CONTROL handoff (additional hardening)

The native backend now has three **runtime primitives**, not yet a completed
compiler/scheduler feature:

- `ActorStateReadWriteGate`: FIFO read/write tickets, bounded readers
  and waiters, generational scope, cancellation and retirement fences.
- `ActorStateReadWriteContinuations`: trusted monotonically identified
  continuation registry; durable Ready/Cancelled status until CONTROL admission
  acknowledges the exact status; terminal notifications survive failed
  enqueue attempts. Registry records including unacknowledged cancellations
  are capped at 9,216 to prevent memory growth when CONTROL is saturated.
- `ActorStateReadWriteControlPump`: serializes bounded nonblocking CONTROL
  admission attempts; accepts a scheduler-provided noexcept receipt callback.
  Failed admission leaves the event pending; it never invokes guest code.

The adapter deliberately **does not use** the reserved supervisor panic slot
as a normal RW wake queue. The production scheduler must provide a separate
bounded delivery path, confirm a receipt only after admission, enforce
single-flight actor execution, check `registry.admitted(id, mode)` at actual
dispatch (retirement can invalidate an already enqueued Ready item), and
ensure all state loans are drained before quiescent retirement. No component
here permits direct guest callback execution or parallel guest actor reads.
Native and Java source lowering/effect checks are still outstanding.


## Additional hardening: actor identity and notification delivery

The continuation registry now requires a unique process-local **actor
incarnation ID** for request, complete, cancel and admitted checks. READY and
CANCELLED notifications carry that ID, and acknowledgement accepts the complete
notification including incarnation and status. A stale READY receipt cannot
authorize an actor whose slot/continuation number was reused following
retirement.

`cancel_pending` permits revoking a queued ticket or an admitted ticket whose
READY notification has **not yet** received CONTROL acknowledgment. It refuses
to cancel acknowledged grants, since the guest callback may be executing.
The trusted scheduler must retire/quiesce for that stronger cancellation.

CONTROL pumping is nonblocking even on recursive invocation and concurrent
pump attempts, using an atomic fail-fast admission flag. Each pump copies at
most its explicit CONTROL delivery budget, not the full cancellation backlog.
Never treat a CONTROL signal/wake as an admission receipt; the actual bounded
queue must accept a typed notification first.

The exact-source native gate/continuation/CONTROL test suites, including
incarnation reuse, missing acknowledgments, cancel-before-dispatch, reentrant
pump, callback/retirement races, backlog saturation, and sanitizer checks,
passed in **private** funded Actions run
https://github.com/agent-pontifex/agent-pontifex-daemon/actions/runs/38312700057.

**Security policy for further CI:** private Oreslang source must only be
mirrored into private test repositories. A prior public test mirror was reset
from its branch tips, but old Git objects/CI records may persist; track
administrator purge in private issue #256.


## Security re-audit: no uncontended admission bypass

All state tickets, **including immediately acquired uncontended Read and
Write grants**, require a separate acknowledged, bounded CONTROL admission
before `ActorStateReadWriteContinuations::admitted` authorizes dispatch.
Owning the logical gate ticket is **not** proof that the scheduler has
assigned the actor's single guest execution lane. `cancel_pending` can
revoke an initial Ready grant while it still awaits CONTROL acknowledgment.

The adversarial tests reject completion before the first CONTROL receipt,
exercise cancellation before first dispatch, and require a real pump
admission receipt before considering an initial grant runnable.

Exact-source private native GCC, Clang ASan and UBSan tests passed in
https://github.com/agent-pontifex/agent-pontifex-daemon/actions/runs/38317005320.
The tests exercise the admission substrate; binding to the production
scheduler and source-level compiler lowering remain independent release gates.


## Scheduler admission and one-shot guest-entry bridge (2026-10-12)

The native scheduler now offers an independently bounded, generation-fenced
`ores_native_actor_scheduler_publish_state_wake` channel with **one typed
state-wake slot per actor**. This channel never consumes the reserved
supervisor CONTROL/PANIC slot. Delivery order across ready actors is:
supervisor CONTROL/PANIC, actor-state READY/CANCELLED wake, then ordinary mail.
All three run under the same scheduler-owned physical execution claim, and
a busy actor cannot execute an additional wake in parallel.

The native C++ `ActorStateReadWriteSchedulerBridge` connects the durable
continuation pump to that scheduler API. The enqueue's successful return
is the exact native admission receipt; failures leave durable notifications
for retry. A READY callback must execute on the owning actor lane, revalidate
its actor reference, and atomically claim the unique continuation using
`registry.try_claim_ready`. The claim permits receiver-before-producer-ACK
ordering, but rejects duplicate READY envelopes. The same actor callback
finishes through `complete_dispatched`, which requires a previous claim.

The original host-level `registry.complete` remains available for the
existing standalone gate tests; production native guest paths MUST use the
scheduler-checked `bridge.complete`. CANCELLED notifications are terminal
bookkeeping only, never permission to enter guest code. The bridge/scheduler
lifetimes require an external producer fence and final drain before destroy.

**Exact-source verification:**
- https://github.com/agent-pontifex/agent-pontifex-daemon/actions/runs/38316943271 —
  new dedicated channel plus five older scheduler regressions passed under GCC
  and Clang, with an ASan/UBSan pass for the new C test.
- https://github.com/agent-pontifex/agent-pontifex-daemon/actions/runs/38317304752 —
  integrated native-C scheduler and C++ continuation bridge passed under
  strict GCC and Clang ASan/UBSan; covers receiver-before-ACK, replay,
  forged actor incarnation and actor-lane-only claims. Runner is private and
  verifies exact Git source blob IDs.

**Still unsupported:** executable source-level `state {}`,
`handle_state(critical-read/write)`, compiler-proved effects and borrow
lifetimes, source-actor codegen integration, and cross-backend conformance.
These runtime host adapters are not themselves a license for concurrent
guest execution or escaping state loans.
