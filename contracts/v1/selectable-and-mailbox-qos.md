# Selectable and priority actor mailboxes — cross-backend target v0 (2026-10-13)

**Status: normative design target, not a claim of implemented guest/compiler support.**
Keep this file byte-identical in Java/GraalVM and LLVM until an explicit versioned
replacement. Both backends must fail closed for unsupported lowering; formatter-only
acceptance, host-only tests, and a Java Future race are not conformance evidence.

## Distinct abstractions, one selection protocol

| Abstraction | Responsibility | How it participates in `select` |
| --- | --- | --- |
| `Channel<T>` | Typed, bounded FIFO/rendezvous transport within one execution domain | `readch ch` reserves a receive; `writech ch, value` reserves admission |
| `ActorMailbox<M>` | Actor-owned, incarnation-fenced message admission and delivery | Controlled inbox receive and outbox send adapters; **not** exposed raw bypass channels |
| `Observable<T>` | Factory of potentially multiple bounded subscriptions | A one-item subscription operation with cancellation and declared delivery policy |
| `ReadableStream<T>` | Exclusive reader and ordered flow control | One-item read operation holding its reader lease |
| `Future<T>` | One eventual completion; not another actor/thread | Readiness of completion; losing the select detaches only its listener |
| `async` / `await` | Callable effects / task suspension, not transport | `when await future` registers readiness **without awaiting first** |
| `readch` / `writech` | Channel operation expressions | Syntax sugar producing selectable read/write operations |
| `Selectable<T>` | A source or operation reducible to **one event** (or the **next** event) of type `T` | Compiler-recognized reduction to one atomic selection candidate |

### Target language-level trait

`Selectable<T>` is a standard, compiler-recognized trait describing
**reduction of a source to one event or its next event**. A Future has a
single completion event (and may be observed repeatedly); a Channel or
Subscription has a cursor, so each successful selection advances that cursor
exactly once. This trait does not make a multi-item container itself an
atomic batch: every `select` arm registers exactly one operation. An empty
or completed source produces a typed terminal indication, never a fabricated
`T` value.

The publicly implementable requirement must be a *pure description* returning
an approved domain-local `SelectOperation<T>` capability. The runtime
registration, reservation, and winner token are sealed and cannot be forged.
Illustrative target contract (not yet valid source on either backend):

```ores
define trait Selectable<T> as
  pub fnc select_next(): SelectOperation<T>;
end
```

A `Selectable<T>` source can yield `SelectOperation<T>` every time it is
selected; one event is **reserved**, not consumed, until that arm wins.
`Future<T>` is level-triggered (its terminal outcome remains observable),
whereas streams, mailboxes, and channels are edge/cursor-triggered and must
preserve losing events. Duplicate observers of one cold Observable may each
begin from its initial value; a loop that requires progressive consumption
must hold one explicit Subscription/reader lease rather than re-subscribing.

An implementation can delegate to vetted operations such as
`SelectOperation.read(channel)`, `write(channel, value)`,
`future(future)`, `subscription_next(subscription)`, or a safe
`map` adapter; user code cannot inject callbacks into a scheduler lock,
fake readiness, or directly issue a commit token. The trait does not itself
transfer ownership between actors. Select operations, channel handles,
subscription leases, and registrations are domain-local and non-sendable
unless an explicit future transport capability is proven.

`Channel<T>` as a resource is **not** automatically a selectable write:
reads and writes have different types and ownership. An optional read sugar
can adapt a channel to `Selectable<T>`; `readch` and `writech` remain the
unambiguous canonical spellings.

## Source syntax target

The same static selection can mix operations from distinct sources:

```ores
const obs = Observable.of([1, 2, 3]);
const fut = Future.of(4);

select {
  when readch foo: int i -> {
    stdio.println(i);
  }
  when obs.take(1): int a -> {
    stdio.println(a);
  }
  when await fut: int b -> {
    stdio.println(b);
  }
  when mySelectable: int c -> {
    stdio.println(c);
  }
}
```

`when <selectable expression>: [binding] -> { ... }` is the new generic
arm grammar. Typed bindings may contain `int i`, `const int i`, or
`const int mut i` as admitted by the final typed-binding grammar.
Legacy `when readch ch: const x { ... }` and `when writech ch, x: { ... }`
stay parseable; `when` is canonical and `case` is compatibility syntax.
The arm result type must unify with the operation's `Selectable<T>` type;
no implicit channel-to-Future, Observable-to-value, or deep structural
runtime type checks are allowed.

An operation whose result is `void` binds no name but still uses the explicit
selectable-arm arrow, for example:
```ores
select {
  when self.outbox.send(command): -> {
    stdio.println("destination transport admitted command");
  }
}
```
The `Selectable<void>` type checker must reject attempts to bind a value;
an operation yielding `T` requires a compatible binding (or an explicitly
discarding syntax) and may not silently erase an incoming item.

`when await fut` is arm-level syntactic sugar for creating the future
completion operation, **not** an early suspension or an additional concurrent
task. A standalone `await fut` still suspends normally. `select` chooses
exactly one committed arm. `nb select` only registers and returns and later
executes the winner on the actor's serialized continuation lane; `try
select` probes without leaving registrations. An arm's `nb`/`try` wait
modifier remains invalid.

`obs.take(1)` is selectable **only if** it exposes a real lossless
one-item operation under the selection protocol. Existing
`rx-oreslang-channels.take(Observable<T>, int): Observable<T>` returns an
Observable *view*, **not** a selectable one-event operation: compiling
`when obs.take(1)` as though it were `Selectable<T>` is prohibited until
an adapter actually owns an atomic reservation. The portable, explicit
source-level target for a progression of items is:

```ores
const sub = obs.subscribe();
select {
  when sub.next(): int a -> { stdio.println(a); }
  when await fut: int b -> { stdio.println(b); }
}
```

Here `sub.next()` means a **described pending next event** on an exclusive
subscription lease, not a blocking `next()` call evaluated before select
arming. Existing `Subscription.next()` currently suspends and returns
`Option<T>`, so this surface is also a **target**, not currently executable
source. If an overload cannot distinguish `next()` unambiguously, introduce
`sub.select_next()` instead and retain `next()` for ordinary pulling.
`obs.take(1).select_next()` can be implemented on a one-item view once
its lifetime/rollback semantics are proven.

A selected `int a` arm runs only on an actual emitted integer. Empty
completion retires the arm without inventing a value; when all arms retire,
selection completes with typed `SelectExhausted`. To observe EOF explicitly,
select `Selectable<StreamEvent<T>>` or `Selectable<Option<T>>`.
Terminal error/cancellation identities are never coerced to an empty value.
A channel/mailbox/Future that is ready at registration may win immediately,
without demanding an item from an unready Observable.

## Atomic heterogeneous arbitration (mandatory)

1. Evaluate arms once, left-to-right, in the owning execution domain;
   transfer/borrow-check outbound payloads and establish finite waiter quotas.
2. Register all candidate operations with **one** selection generation and
   one winner cell. Registration is an arming step, not a consuming read.
3. Each source supports `reserve/probe -> commit` under the common winner
   protocol. Readiness is a reversible reservation; losers release without
   consuming, writing, acknowledging, or refunding admission twice.
4. The selected operation commits its payload exactly once. An independently
   completed Future result is immutable and may be observed without consuming
   it; a losing Future arm merely detaches the listener and does not cancel a
   shared Future or its producer.
5. A stream/observable adapter must own a bounded pending-item reservation
   or an upstream rollback/demand protocol. It cannot simply subscribe to a
   hot producer and discard values when another arm wins. If a publisher
   cannot provide lossless reservation, it must explicitly advertise its
   lossy policy or be rejected as `Selectable<T>`.
6. Cancellation/stop/reload seals the selection generation, removes all
   registrations, releases reader leases and reserved quotas, and forbids any
   late callback from executing a guest branch. Cleanup is idempotent.
7. No operation parks a scheduler carrier; all source callbacks only post a
   continuation to the actor's shared scheduler, never execute guest code
   directly on I/O or Future completion threads.
8. Default `select` is deterministic FAIR round-robin among *observed*
   ready cases. `select first` is explicit static priority and
   `select random` is explicit random. A subsequently-ready higher-priority
   arm cannot retroactively steal an already committed winner.

`SelectCase`, `SelectSet`, `SelectPlan`, static arms, and the dynamic
`select from` form must use the **same** arbitration. Merely implementing
`Future.anyOf`, registering channel consumers speculatively, or adding an
ad-hoc polling loop is forbidden.

## Actor mailbox QoS and control progress

Mailbox delivery and select-case priority are different concerns. Each actor
has a logically single inbox; its mailbox consists of distinct **bounded
admission lanes** feeding one actor-owned serialized dispatcher:

- `CONTROL`: runtime-owned STOP, CANCEL, lifecycle, supervisor terminal,
  and continuation-wakeup authority; user code cannot enqueue CONTROL mail.
  Stop/cancel admission and scheduler wakeup must also have an out-of-band
  path so a saturated user queue or unavailable guest consumer cannot block
  termination.
- `URGENT`: permission-checked application priority with small separately
  reserved slots and per-sender/actor quotas, never convertible to CONTROL.
- `NORMAL`: bounded normal messages.
- `BULK`: bounded background messages, throttled more aggressively.

Control headroom is separate from user credits. Every lane has finite byte,
item, waiter, and in-flight accounting, including payloads checked out by
handlers and callbacks already scheduled. Admission failure supplies explicit
backpressure (`try_send` fails or an awaitable sender suspends), not silent
drop and not unlimited producer waiters. Self-send deadlock on a serial lease
must fail closed. Ordinary `Channel<T>` stays FIFO; mailbox QoS lives in
`ActorMailbox`, not in the underlying channel API.

STOP/CANCEL/restart fencing is out-of-band runtime authority and cannot
be queued behind any guest message. When no higher-level handler is active,
continuation wakeups are preferred, but must have a bounded dispatch burst:
a perpetually refilled continuation queue must not starve user work.
A suspended serial/source handler is the exception: its continuation keeps
the logical handler lease, so another user message cannot run until it ends.
The dispatcher then schedules URGENT/NORMAL/BULK with **bounded starvation**
(e.g., quota-limited weighted deficit round robin, age promotion and
non-starvable NORMAL/BULK service). Within a lane, stable per-sender FIFO is
maintained; strict global FIFO across priorities is explicitly not promised.
An urgent message cannot preempt a currently executing guest handler:
scheduler cooperation/reduction checkpoints, bounded handler work, and
dedicated continuation/control wakeups are required for responsive actors.
A serial actor still has at most one *logical* message lifetime; urgent
user messages cannot bypass a suspended serial handler's lease. Runtime
CONTROL cancellation may terminate that lifetime without becoming an
additional guest handler.

The outbox is a policy/transport endpoint with bounded credits and explicit
destination/sender authority, not a second unbounded staging queue. A
`self.outbox.send(mail)` selectable means **admitted to destination
transport**, never 'handler finished'; `mail.done()` / typed reply completes
processing. `self.inbox.receive()` is actor-owned only and may never bypass
private/shared/untrusted envelope validation or consume continuation CONTROL
envelopes as user messages. Supervisors receive terminal events on reserved
CONTROL paths, even when user inbox/outboxes are saturated. Priority forging,
cross-actor channel capability transfer, and control-plane amplification are
rejected at admission.

## Conformance gate and known implementation gaps

Required cross-backend guest tests: mixed channel/observable/Future selection;
typed bindings and legacy syntax; finite ready-sets and fairness; atomic
rendezvous with competing completion; loser never steals channel data or
publishes an outgoing write; repeated `nb select` under actor single-flight;
cancel/STOP/close vs commit races; future failure and empty observable; bounded
registration quotas; hot publisher with/without lossless reservation; stream
exclusive-reader enforcement; dynamic/static selection parity; no carrier
blocking; priority flood with bounded control latency, fair normal/bulk service,
sender FIFO, and no user CONTROL spoofing; saturated in/outboxes with
cancellation and supervisor STOP. Include parser, type/ownership checker,
formatter, Graal guest, LLVM guest, and sanitizer/model checks.

**As of this design:** Java's `ChannelRuntime.SelectSet` registers only
channel read/write/default cases and uses one coordinator; its source parser
also builds channel-specific static select AST arms. Existing Java channel
and actor tests do not prove heterogeneous select or priority-mailbox QoS.
The reactive helper library currently returns observables from `take`,
not a `Selectable<T>` registration. LLVM's documented stream contract
explicitly lacks full source-level Channel/Observable parity. These are
implementation blockers, **not** features to mark as passing.
