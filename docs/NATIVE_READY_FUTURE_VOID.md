# Narrow typed actor-readiness Future<void> source lowering

**Scope: one live ActorRef implementing HasFuture, optionally bound through
one real source-level Future<void> readiness token, and one readiness await.**

```oreslang
const worker: ActorRef<Worker> = spawn Worker();
await worker; // HasFuture.get_ready(), not get_done()
worker.send(1);
stop worker;
return 43;
```

Three admitted spellings have identical readiness semantics:
`await worker;`, `await worker.get_ready();`, and
`const Future<void> ready = worker.get_ready(); await ready;`.
The direct operand must resolve to a **live ActorRef<T>** with HasFuture
readiness; the restricted LLVM compiler does **not** yet implement general
HasFuture trait/interface dispatch for arbitrary objects. Awaiting a worker
is not awaiting its `get_done()` lifecycle completion.

The compiler represents the typed lexical Future as a non-Copy, actor-affine
capability, not an int or heap pointer.
The ready Future is consumed once, before all post-await sends and stops,
into the existing compiled root CONTROL continuation. The execution uses
`oreslang_source_actor_await_ready` with its continuation provenance and
scheduler-generated CONTROL turn; the actor carrier does not spin or block.
The surrounding actor API remains the restricted one-actor, zero-suspended-
local, scalar-return slice. The token may not be copied, shadowed, reassigned,
passed as a parameter, returned, or sent between actors.

This is **not** general `Future<T>` or `async fnc` lowering. In particular,
there is no compiler-owned scalar result Future, suspendable local scalar,
multiple await state machine, arbitrary Future producer, typed cancellation
or channel/select interoperability yet. `Future<int>` and unsupported
Future producers remain fail-closed before IR generation.

New compiler contract fixtures assert positive IR for both the generated
`ready_continuation` and the existing native await runtime, plus rejection
of unused, mismatched, duplicate, unspawned, shadowing, or multiply awaited
ready tokens. A native AOT Rust regression exercises all three spellings
and asserts the same message effects and logical ancestry. Invalid
non-ActorRef operands and unsupported lifecycle methods fail closed.
Actual native compiler, AOT runtime and Java/GraalVM differential execution
are **required separately** and are not established by writing these tests.
