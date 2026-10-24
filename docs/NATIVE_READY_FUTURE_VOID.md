# Narrow typed actor-readiness Future<void> source lowering

**Scope: one real source-level Future<void> binding to a previously spawned
ActorRef's get_ready() and one await on the binding.**

```oreslang
const worker: ActorRef<Worker> = spawn Worker();
const Future<void> ready = worker.get_ready();
await ready;
worker.send(1);
stop worker;
return 43;
```

The compiler represents this particular non-Copy, actor-affine lifecycle
Future as an opaque checked lexical capability, not an int or heap pointer.
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
ready tokens. The existing direct `await actor.get_ready()` remains admitted.
Actual native compiler, AOT runtime and Java/GraalVM differential execution
are **required separately** and are not established by writing these tests.
