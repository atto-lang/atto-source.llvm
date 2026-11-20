# Callback delivery and execution-context model

Status: proposed common language contract, with a tested **Java host-runtime**
event adapter. The `sync` / `deferred` callback effects below are specification
notation, not newly implemented source syntax. No source `HttpMessage.on_headers`
implementation or five-backend source parity is certified by this change.

## Delivery is an API effect

An API must declare one delivery mode for each callback parameter; it must not
choose a mode based on cached results, fast paths, errors, or contention.

| Effect | Required behavior |
| --- | --- |
| `sync` | Every invocation is nested in the registering logical call; no callback escapes after that call returns. If a result is unavailable, return an explicit pending/error result or reject the operation. Never silently defer. |
| `deferred` | Registration can settle an operation immediately, but invokes no guest callback. Accepted delivery uses the owner's event lane after its registering logical handler finishes and the previous physical carrier exits. Never silently inline. |
| runtime `resume` | Private compiler/runtime operation continuing an existing logical invocation at a saved program counter. It may execute while that invocation remains admitted. It is not an independent event. |

Callback delivery and callback body suspension are separate effects. An `async`
body can start inline until its first await; an `async` annotation alone does not
specify deferred delivery. A lambda annotation must be checked against the API
parameter type, including through variables, wrappers, higher-order calls and FFI.
A mixed-timing foreign API needs an adapter, not a cast to `sync`.

For HTTP use a deferred event subscription or a Future/await API. If synchronous
inspection is needed, expose a separate operation such as `try_headers()` that
returns an option. Marking a callback `sync` cannot make future network data ready.
A sync callback must be non-escaping and cannot contain an unlowered suspension;
its effects propagate transitively to helpers. An explicit bounded no-yield
region is a separate scheduling constraint, not an implication of `sync`.

For a delivered deferred event, the example's order is **first, second, third**.
This means after the *logical* handler tail, not merely after registration returns
or a physical thread yields. Delivery is at most once for a one-shot operation;
cancellation, actor stop or overload may prevent delivery. These outcomes must
be visible through the operation/subscription lifecycle; do not promise execution
of a success callback after cancellation. Multi-shot streams need a separate
subscription and backpressure contract.

## Logical invocation and physical carrier

A logical ExecutionContext owns continuation PC, captured values, actor identity
and generation, cancellation, cumulative fuel/wall/CPU accounting, and its
unwind/defer/recover chain. Carrier-local storage only borrows the context pointer
while that carrier owns the actor execution lease, then clears it in `finally`.
Awaited calls retain the same logical unwind chain; an independent event gets a
new invocation context and cannot recover the registering handler's failure.

Automatic CPU cooperation saves the continuation, exits the guest VM boundary,
then releases the physical lease. It retains logical handler admission. Only
that handler's resumes can advance it; independent events and messages wait.
For serial actors this exclusion also persists over genuinely pending awaits.
A future reentrant actor policy must explicitly define which events may enter at
an await; automatic preemption alone never grants that permission.

The Java adapter adds a bounded event lane distinct from the priority resume
lane. `enqueueEventOnCompletion` publishes an event;
`enqueueOnCompletion` remains internal resume plumbing. Events and resumes share
the existing 1,024-slot internal quota, separate from user mailbox capacity.
Overflow fail-stops the actor; it never executes a callback inline to make room.
Events queued while a carrier is active force exit of its full TurnExecutor
boundary, including the foreign-producer race at the next mailbox poll. Events
are not runnable while `sourceReceiveSuspended` is true. Stop closes and drains
both lanes and retires each reservation once. Targets are runtime-affine.

This host adapter executes a one-shot synchronous callback. It does not implement
source callback effects, durable source event ExecutionContexts, async event-body
lowering, complete unsubscribe/error reporting, or stream subscriptions. An event
blocked behind a serial receive cannot be used to resume that receive: use a
Future's runtime resume path for awaited I/O to avoid self-deadlock.

## Foreign callbacks, lifetime and memory

Completion threads may settle an operation and publish bounded tickets, never
execute guest code or copy ambient carrier authority. Publication establishes a
happens-before edge; captured header data must be owned/frozen or copied before
the producer reuses it. A callback annotation cannot extend a borrowed pointer's
lifetime. Mutable loans, no-yield guards and OS locks cannot cross suspension.

Success, failure, cancellation, timeout and close compete for one terminal state.
Actor identity/generation and invocation identity must be validated at admission;
late completion cannot revive retired state. Cancellation settlement is distinct
from physical quiescence. Retire frames, loans and resources only after the
carrier and foreign producer ownership have both quiesced.

## Uncooperative execution

A watchdog requests preemption; it does not revoke a running carrier's lease.
Compiler-lowered bounded safepoints can cooperate without replaying committed
effects. An arbitrary blocking FFI call or infinite uninstrumented loop cannot
be safely transferred to another thread by changing an ownership bit.

The safe shared-runtime behavior is to retain the lease and reject further work
for that actor until the carrier exits. This preserves isolation but does not
prove termination, fairness or a hard deadline. A bounded termination guarantee
requires admission into a separately revocable process/sandbox with bounded host
resources, and acknowledgment of revocation before reclamation. Reject such a
guarantee when no isolation revoker is available. Do not place undeclared blocking
or unbounded foreign code on the general cooperative pool.

## Backend admission

| Backend | Current evidence and remaining obligation |
| --- | --- |
| Java/GraalVM | Host event lane and adversarial runtime tests; existing source-loop cooperation tests. Callback effects and source HTTP lowering remain unimplemented. |
| LLVM | Existing native lease/continuation substrates; source callback/await lowering remains uncertified. Apply these rules before enabling it. |
| .NET | Arithmetic compiler and host prototypes; no source async actors. Reject unsupported callback syntax. |
| Wasm | Restricted pure scalar ABI; no source async actors or host callback imports. Keep that admission boundary. |
| Forge | Arithmetic bootstrap and mailbox prototype; no source async actors. Reject unsupported callback syntax. |

## Verification and proof scope

Java `docs/formal/CallbackEventAdmission.tla` models one serial invocation, two
possible carriers, up to two CPU yields, pending await/wake, independent event
settlement, handler tail, physical exit and stop. TLC exhausts 124 reachable
states. It checks no event before the logical tail, no inline event, and lease
retention through stop. Three deliberately broken protocols must produce
counterexamples. Existing actor exclusivity and dispatch-ticket models cover
additional actor/carrier and stale-ticket cases separately.

The inductive safety argument is:

1. Initially there is no physical owner and no event entry.
2. Only atomic claim grants physical authority, requiring an empty lease and
   no active physical owner. Yield, completion and cancellation never grant it.
3. A yield preserves logical admission. Only physical exit releases its lease;
   stop preserves a live owner until this exit.
4. Event admission requires a finished logical handler and no physical owner.
   Therefore it cannot enter before the tail or on the prior carrier stack.
5. One terminal event consumes its ticket; stop rejects remaining tickets.

These arguments prove properties of the abstract protocol under its atomicity
assumptions, not that every compiler, FFI path or production runtime refines it.
Implementation refinement still requires checking every entrypoint, context
migration, generation fencing, cleanup, error and resource-lifetime path. No
liveness theorem is claimed without scheduling fairness and terminating code.

Test CLI status (`atto test`, legacy `oreslang test`): no runtime currently
feeds this protocol to the test CLI automatically. `atto test --race --trace
FILE.jsonl` analyzes an explicitly supplied, offline `ORES_RACE_V1` trace (the
Java `RaceTraceRecorder` can emit one from caller-supplied events); live
`atto test --race` exits 2 (inconclusive) until trusted runtime instrumentation
exists. `atto test --fuzz` mutates stdin bytes for a configured executable; it
does not mutate completion mode, yield count or cancellation. Java's
`NoZalgoCallbackDispatchTest` is a host unit test of this ordering, not an
end-to-end race or fuzz gate, and there is no `scripts/no-zalgo` project or
`NO_ZALGO_VERIFICATION.md`.
