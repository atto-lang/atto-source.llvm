# Narrow typed actor-readiness Future<void> source lowering

**Scope: one live ActorRef implementing HasFuture, optionally bound through
one real source-level Future<void> readiness observer, and one logical
readiness barrier that may be observed repeatedly.**

```oreslang
const worker: ActorRef<Worker> = spawn Worker();
await worker; // HasFuture.get_ready(), not get_done()
worker.send(1);
stop worker;
return 43;
```

Readiness can fail if the actor terminates before READY publication, so the
enclosing native source function must declare `throws ActorTerminated`.
`get_done()` remains the separate physical-finalization barrier.

Three admitted spellings have identical readiness semantics:
`await worker;`, `await worker.get_ready();`, and
`const Future<void> ready = worker.get_ready(); await ready;`.
The direct operand must resolve to a **live ActorRef<T>** with HasFuture
readiness; the restricted LLVM compiler does **not** yet implement general
HasFuture trait/interface dispatch for arbitrary objects. Awaiting a worker
is not awaiting its `get_done()` lifecycle completion.

The compiler represents the typed lexical Future as an immutable, non-escaping,
actor-affine lifecycle observer, not an int or heap pointer. The binding must be
`const`; mutable `val Future<void>` and `let Future<void>` forms fail closed.

Repeated awaits of the exact same ActorRef readiness barrier or the exact same
bound `Future<void>` observer are non-consuming. In this restricted slice they
are coalesced onto one generated root CONTROL continuation. Re-observing the
same barrier through the producer ActorRef is equivalent. This does not imply a
general multiple-suspension state machine or authorize a second ActorRef
readiness barrier.

The execution uses `oreslang_source_actor_await_ready` with continuation
provenance and a scheduler-generated CONTROL turn; the actor carrier does not
spin or block. The surrounding actor API remains the restricted one-actor,
zero-suspended-local, scalar-return slice. The observer may not be copied,
shadowed, reassigned, passed as a parameter, returned, or sent between actors.

This is **not** general `Future<T>` or `async fnc` lowering. In particular,
there is no compiler-owned scalar result Future, suspendable local scalar,
general multiple-suspension state machine, arbitrary Future producer, typed cancellation
or channel/select interoperability yet. `Future<int>` and unsupported
Future producers remain fail-closed before IR generation.

Compiler contract fixtures assert positive IR for the generated
`ready_continuation` and existing native await runtime, including repeated
direct, explicit-get_ready, bound-observer, and mixed observer/producer
observations of the same barrier. They continue to reject unused barriers,
mismatched or duplicate producers, unspawned receivers, mutable bindings,
shadowing and escaping observers. A native AOT Rust regression exercises all three spellings
and asserts the same message effects and logical ancestry. Invalid
non-ActorRef operands and unsupported lifecycle methods fail closed.
Actual native compiler, AOT runtime and Java/GraalVM differential execution
are **required separately** and are not established by writing these tests.
