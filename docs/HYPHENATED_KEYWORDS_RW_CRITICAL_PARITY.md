# Hyphenated compound keywords and reader/writer critical declarations

Status: **language contract only; executable compiler/runtime support is not established by this document**.
Implement and verify separately in Java/GraalVM and LLVM/OresVM before advertising the
examples as runnable. This contract is intended to align with Java critical
callable PR #577, Java actor-pipeline issue #643 / PR #640, Java rt-proxy
nonparking RW-lock issue #336, and LLVM native CriticalGate.

## Lexical contract

The hyphen is the ASCII hyphen-minus **U+002D** (`-`), not a Unicode dash.
Only explicitly enumerated *compound keywords* are recognized; **do not**
make `-` part of ordinary identifiers.

| Compound spelling | Compatibility spelling | Context |
| --- | --- | --- |
| `transform-pipe` | `transform pipe` | Actor role modifier, before `actor` |
| `nb-select` | `nb select` | Nonblocking static/dynamic select expression/statement |
| `critical-read` | no pre-existing read equivalent | Callable declaration modifier |
| `critical-write` | `critical` | Callable declaration modifier |
| `critical` | unchanged | Exclusive callable modifier |

Unicode en/em dashes are not equivalent to U+002D and must not be
silently normalized. A source format pass must never convert subtraction
into a compound keyword. The Java compiler may tokenize `nb-select` as
distinct adjacent `NB SELECT` tokens when source positions remain accurate.

A compound keyword has no whitespace around the hyphen. It must match
*exactly* and in a supported grammar position: `critical - read` is not
`critical-read`; `critical-readonly` is not a keyword. Do not greedily
combine names/keywords in arbitrary expressions, particularly `a-b`,
`a - b`, `a--b`, `x-1`, `foo->bar`, or negative numbers. Keep source
ranges/columns accurate, including the hyphen, for diagnostics and formatter
round-trip. Formatter should prefer the hyphenated spelling for compound
keywords and preserve user-chosen `critical` as an accepted write alias.
A token-based implementation may recognize adjacent
`IDENT/KEYWORD MINUS IDENT/KEYWORD` only in the designated parser context;
it must not change arithmetic tokenization.

## Surface examples (proposed; not all currently executable)

```ores
define transform-pipe actor Foo as
    // Role-admission and stream lowering are governed by the pipeline contract.
end

pub fnc critical-read read_balance(): int {
    return balance;
}

pub fnc critical-write update_balance(value: int): void {
    balance = value;
}

pub fnc critical update_balance_compat(value: int): void {
    balance = value;
}

// Equivalent to nb select { ... }:
fnc consume(Channel<int> events): void {
    nb-select {
        when readch events: const event {
            handle(event);
        }
        default: {}
    }
}
```

Future compiler-enabled forms must accept both
`pub fnc critical-read name(...)` and `pub critical-read fnc name(...)`,
matching the callable modifier-permutation policy. Instance method forms
(bare name, `method`, legacy `fnc`) need the same semantic mode.

**Current admission:** Java accepts the `nb-select` shorthand only;
`critical-read`, `critical-write`, and `transform-pipe` are reserved
and rejected until checked runtime lowering exists. LLVM reserves the
compound spellings but rejects unsupported native guest-source lowering.
Do not interpret parsing or negative tests as runtime support.

## Reader/writer scope

Every protected *domain* owns **one** RW admission gate, not separate read
and write gates. Match existing critical owner scopes:
- instance methods: per-object gate shared by all critical modes on that object;
- file/module functions: per-code-unit, per-execution-context gate;
- static class functions: per-declaring-class, per-execution-context gate.

No process-wide global lock across tenant isolates/VMs. Additional explicit
shared-resource gate binding is future work and must use trusted ownership
capabilities; annotating an arbitrary function does not magically protect
external shared data.

| Permit | Allowed together |
| --- | --- |
| `critical-read` | Other read permits on the same domain |
| `critical-write` | No readers or other writers on that domain |
| `critical` | Exact alias for `critical-write` |

Ordinary (unannotated) methods are **not** implicitly serialized by the
gate. The type/effect/ownership checker must reject unguarded access to
state that is declared protected by this domain. Merely parsing the modifier
or wrapping one method in a lock is not a memory-safety guarantee.
`critical-read` forbids effects that mutate its protected state, including
indirect writes through aliases/calls; mutating unrelated isolated locals
is allowed. An unknown call effect fails closed. Actor single-flight
execution remains the actor's independent guarantee: reader permits do
not grant concurrent execution if that actor itself admits only one turn.

## Admission, fairness and completion

Use logical invocation/continuation IDs, not JVM `Thread`s, pthread IDs,
or physical carrier identity. On contention, queue a bounded cancellable
waiter and **suspend the logical continuation**, releasing the carrier;
resume admitted work through a scheduler-controlled resume path (not as
inline guest code under the gate mutex). A synchronous caller that cannot
suspend must fail closed on actor-carrier contention rather than park it.
Prefer `suspend critical-read` / `suspend critical-write` for functions
that may contend and need resumable admission.

A short metadata lock may serialize queue operations, but guest code never
executes under that host lock. Use FIFO writer-aware cohorts:
- an idle gate may admit a reader immediately;
- multiple readers may hold simultaneously until a queued writer forms
  the fairness barrier;
- readers arriving behind a writer must queue, even while earlier readers
  still hold permits;
- when active readers exit, admit the next FIFO-eligible reader cohort,
  or grant the head writer **only after the final active reader releases**;
  reader cohorts must never bypass a queued earlier writer;
- no writer may run while any reader is active; no two writers overlap.

Queues are bounded, have explicit overflow failure and do not starve
queued writers under continuous read traffic. Exactly-once close releases
permits on return, throw, raise, panic, cancellation, STOP/teardown, and
runtime closure. A cancelled *granted but not yet resumed* waiter must
release its grant and wake the next cohort. Track `queued`, `granted`,
`running`, and `released` states explicitly; exactly one path owns the
transition to `released`. A grant/resume race must not leak permits or
run guest code after cancellation or incarnation retirement. Do not hold the metadata mutex
while completing futures or enqueueing scheduler work. Admission and
teardown require generation fencing against stale resumes.

Nested same-logical-continuation `read->read`, `write->write`, and
`write->read` may reuse their owning ticket with lexical depth, **but**
a nested read under a writer retains writer exclusivity until the outer
write scope exits. A nested call in another continuation is not reentrant
even when the actor ID is the same.
`read->write` upgrade must be **rejected** to avoid two-reader deadlock;
do not implicitly release the read gate and reacquire write because that
loses atomicity. Cross-domain nested acquisition needs lock-order /
wait-for-cycle checks; distinct logical continuations must never inherit
another invocation's permit.

## Suspension and state loans

A *logical admission permit* is not an OS mutex, and the `suspend`
callable model may retain it across a compiler-lowered await only when
the continuation, cancellation, and ownership contracts are proven.
That does **not** authorize carrying a mutable/reference actor-state
loan through suspension. Keep the existing strict `handle_state`
loan-fencing and effect restrictions. Never create an implicit await
inside an ordinary synchronous `critical-read` or `critical-write`
call: type/effect-check the suspension boundary.

Reader/writer critical regions do not change supervisor CONTROL ordering,
actor incarnation isolation, or actor panic routing. A task's permit must
never be stolen or shared by a second actor continuation.

## Cross-backend acceptance gates (all required)

1. Lexer/parser/formatter golden vectors for all four hyphenated spellings,
   modifiers before/after `fnc`, and arithmetic/arrow/whitespace near-misses.
2. Type/effect/borrow tests: read-only vs protected writes, transitive
   mutation through callees, unknown effects, actor-state loans, and non-
   suspending calls that might block.
3. Scheduler stress with readers > carriers, writers > carriers,
   writer fairness under a hot read stream, and per-domain independence.
4. Linearizable read/write histories: never R+W or W+W overlap;
   simultaneous reads permitted when the actor admission policy allows.
5. Cleanup tests for queued and granted waiter cancellation, panic,
   exception, double release, queue capacity, shutdown and incarnation
   generation changes.
6. Deadlock tests for recursive same-domain calls, illegal read->write
   upgrade, A->B->A across domains, and parent/child logical tasks.
7. Formal state machine: enforce writer exclusivity and reader/writer
   exclusion as invariants; model-test lost wakeups, reader cohorts and
   cancellations; include intentionally unsafe transitions that yield
   counterexamples.
8. Run the **exact source revision** through executable Java/GraalVM and
   LLVM tests, with native sanitizer runs where applicable. LLVM's existing
   CriticalGate native test alone does not prove guest source support.

Until these gates pass, do not silently treat `critical-read` as
`critical`, treat `critical-write` as a plain function, or claim
`transform-pipe` is an executable pipeline. Unsupported lowering must
produce a clear diagnostic and no partially generated output.

## Current implementation staging and effect contract (October 12, 2026)

The lexical Java draft is [#755](https://github.com/ores-truffle-oreslang/oreslang-source.java/pull/755);
the native fail-closed parser draft is [#254](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/254).
A first Java runtime **admission substrate** is being developed in
[#780](https://github.com/ores-truffle-oreslang/oreslang-source.java/pull/780),
stacked on the existing logical exclusive critical gate
[#577](https://github.com/ores-truffle-oreslang/oreslang-source.java/pull/577).
It does not enable these source-level modifiers or integrate compiled
protected-state access.

Resolve the actor-state grammar against canonical Java
[#760](https://github.com/ores-truffle-oreslang/oreslang-source.java/pull/760)
and [#765](https://github.com/ores-truffle-oreslang/oreslang-source.java/pull/765)
and LLVM [#257](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/257)
and [#259](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/259):

- `with state.read[, state.write]` is a verified effect upper bound; it
  does **not** grant capabilities or acquire a synchronization permit.
- `using state.read(...)` and `using state.write(...)` are **lexical**
  state capabilities, independent of a callable's reader/writer gate.
- A declaration `critical-read` may not infer `state.write` or grant
  state-field access outside a scoped `using` loan. A declaration
  `critical-write` alone does not grant `state.write` either.
- Actor turns remain single-flight; a reader gate cannot silently schedule
  two guest actor turns simultaneously. Parallel reads require independent
  verified concurrent-admission metadata, not just a method modifier.
- No live actor-state loan may cross suspension simply because a logical
  critical ticket can survive it. The mode/effect/ownership proofs and
  cancellation/supervision checks remain separate release gates.

Phase-one Java `CriticalRwGate` admits multiple detached read tickets,
a single writer, FIFO reader cohorts, and asynchronous waits. It currently
**fails closed on all same-logical-task nested acquisitions**, including
read->read and write->read, until reentrancy and cross-primitive lock-order
proofs are implemented. These are future target semantics above, not
claims of existing behavior.

Exact-Git-blob validation of the hardened RW admission substrate passed
on a private funded runner:
[run #38323495940](https://github.com/zed-pkg-test/oreslang-private-pr-validation-20261012/actions/runs/38323495940).
The workflow checked exact production Git blobs of
`CriticalRwGate` `b594da55830eafffe9cc35c5a8a41f271e150ae9`,
`OresFuture` `121a739016cab93072371564fc93290b06825196`,
`OresScheduler`, `Awaitable` and `MixedInteropBridge`.
It compiled these together with a **test-only ActorRuntime shim** and
exercised FIFO cohorts, writer exclusion, bounded readers/queue,
cancelled-writer liveness, 400 cancellation/grant races, 16,000 seeded
state transitions, scheduler single-carrier nonparking, cancellation
of a waiting task, and rejection of blocking pending Future reads on
scheduler carriers. The actual actor runtime and full Java build were
**not** part of this isolated test. No full guest compiler effect checking,
Graal Native Image/AOT, LLVM guest parity, formal proof, or ready-to-merge
status is claimed.

## October 12: executable Java legacy-write route (integration WIP)

Java [PR #780](https://github.com/ores-truffle-oreslang/oreslang-source.java/pull/780)
now changes the interpreter's **legacy `critical`** admission at
file-global, per-instance and per-class/static scopes to
`CriticalRwGate.newCriticalTicket()`, equivalent to `Mode.WRITE`.
All such compiler-generated callable routes in
`OresEvalRootNode` use the same kind of gate that future verified
`critical-read` and `critical-write` callables must share. Explicit
`OresMutex.Critical` remains a separate, user-created mutex object;
it must not be mistaken for a compiler-owned protection domain.

The production `OresScheduler` on this PR now **defers settling a
running task's cancellation Future until the executing carrier turn
has completely unwound**. This is essential: `releaseOnCompletion()`
must never release a writer ticket while guest instructions are still
running. The cancellation request can initially return `false` when
a guest turn is active; the task's Future subsequently settles as
cancelled following the carrier unwind. Pending-task cancellation
still settles directly. Tests must check both paths.

The isolated funded exact-source Java run
[38326862701](https://github.com/zed-pkg-test/oreslang-private-pr-validation-20261012/actions/runs/38326862701)
passed with exact production `CriticalRwGate`, `OresFuture`,
`OresScheduler`, `Awaitable` and `MixedInteropBridge` sources,
a **test-only actor shim**, mixed legacy-vs-read/write admission tests,
single-worker suspension, cancellation-before-release behavior, and
the prior stress model. This **does not** exercise the entire 9k-line
guest interpreter / full GraalVM Maven suite, verify actor STOP
incarnation fencing, or enable new source-level RW declarations.
The source repository checks were not verified green. Keep draft.
