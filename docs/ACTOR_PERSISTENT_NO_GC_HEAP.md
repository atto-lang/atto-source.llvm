# Persistent actor-owned native heap — initial OresVM ABI

This extends the strict no-tracing-GC LLVM memory work beyond a
single function activation. `ActorOwnedI64Heap` is allocated once per
native actor incarnation, keyed by host-owned `(actorId, incarnation)`,
and remains alive across admitted actor turns and physical carrier migration.

## API and exact guarantees

- `open(owner, incarnation, capacity)`: bounded 1–4096 scalar slots,
  C-native allocation, opaque heap; domain identity is monotonic and unique
  within a process until wrap (where admission aborts).
- `enter(heap, owner, incarnation)`: verifies owner and creates a distinct
  epoch token. Only one admitted turn per actor heap; a concurrent/reentrant
  turn is rejected. The next physical thread may enter after the old one leaves.
- `new/read/write/drop`: actor-turn-only access to i64 slots through
  generational non-pointer handles. Erase zeros the slot and invalidates the
  old generation. Outstanding loans prevent drop.
- `borrow`: creates a bounded unique loan token that survives
  `leave` and may represent a suspended continuation or FFI lease.
  A loan alone cannot dereference data; it must resume an admitted turn.
- `retire`: fences new turns/guest access immediately. In-flight turns
  can still `leave`, and outstanding loans may still release.
- `try_destroy`: returns pending while an admitted turn or lease exists.
  Once quiescent, clears the actor heap and physically frees native memory.

**No tracing collector** is linked to this ABI. Native `calloc/free`
may incur allocator/page-fault stalls, but the runtime has no guest
GC roots, statepoints, reference-count cycles, or collector pauses.

## Non-negotiable security and integration gates

The native `OresActorI64Heap*` is a **trusted supervisor capability**.
The actor ID/incarnation and turn credentials are not secrets: do not
pass this C ABI directly to untrusted source guest code, FFI, or messages.
Only the scheduler may mint valid turn epochs and control retirement.
Use compiler-generated typed accessors so source programs cannot forge
heap pointers or mutate metadata.

The ABI presently enforces **serial execution** only. An actor with
`requestConcurrency > 1` or `messageConcurrency > 1` needs a documented
execution barrier and ownership rules before using it; do not assume
multiple handlers can read/write an individual native slot concurrently.
A bounded native spinlock guards short ABI control-plane critical sections;
no guest-language callback executes with it held.

`try_destroy` requires the supervisor to have fenced stale callbacks,
timers, wakers, async resumes and external references to the opaque heap.
The runtime checks turn/loan counters, but it cannot recognize arbitrary
native pointers held by dishonest FFI code. Never call into an already
freed heap; a stale raw host pointer cannot be made safe by a generation ID.

This is a **tested native runtime substrate**, not full compiler lowering.
The LLVM parser supports only the restricted i64 actor-field fragment
specified below. It still rejects general actors, async/await, `trap`,
class/struct layouts, and nontrivial destructors. The earlier compiler path lowers `owned int` into a
*function-scoped* native region; a future compiler bridge must select this
actor-persistent domain through an unforgeable ActorId execution context.

## CI

`native_actor_owned_heap_quiescence` exercises multiple turns, cross-thread
carrier migration, suspended loans, retirement pending and eventual physical
free, as well as SIGABRT rejection of foreign actor identity, stale
turn epochs, forged slot domains, drop-with-loan, duplicate loan release,
concurrent reentry and access after retirement. The funded workflow verifies
exact source Git blobs before running the full CTest matrix.


## Experimental source-level actor fields (compiler fragment)

LLVM now accepts a restricted define actor NAME as ... end fragment:
up to 4096 let/const/val FIELD: int = pure-int-expression; fields, and
fnc/routine methods with int arguments, simple self.FIELD assignments and
one integer return expression. The compiler rejects unknown or duplicate
fields, reserved lifecycle symbol collisions, and unsupported syntax.

Generated ores_actor_NAME_init / ores_actor_NAME_METHOD /
ores_actor_NAME_drop are trusted **host** entrypoints. They accept an opaque
heap pointer and flattened scheduler-issued domain/epoch arguments. No
Oreslang source can obtain or construct heap pointers, actor identities or
turn credentials. The real native runtime verifies provenance and epochs.

Tests now include compiled LLVM IR and a linked native host harness that
creates fields, mutates them on a subsequent carrier, and explicitly drops
the record before quiescent heap retirement. They must execute successfully
on a funded runner before a passed validation claim is made.

Remaining limitations: no spawn/ActorGroup integration, automatic
panic/cancellation cleanup, await continuations, FFI completion fencing,
arbitrary class/struct layouts, non-i64 field data or general actor grammar.
Drop remains a host lifecycle obligation, not automatic all-exit cleanup.


## Supervisor-owned persistent source fields and async pins

The new `ActorGroupOwnedHeap.hpp` bridge **composes** the canonical
`ActorGroupLifecycle.hpp` from LLVM main; it does not add another
worker registry. One managed worker owns its opaque native heap, generated
record and the host-only `get_ready()` / `get_done()` tickets.

Under the limited i64 actor compiler fragment, generated host-only
`init` runs in an admitted exclusive turn, `with_turn` gates method
reads/writes, and normal stop, cancellation or caught C++ host exceptions
queue a parent CONTROL request. The parent's `process_control` must
execute on its exclusive logical lane. It waits for BOTH native field
loans and host continuation/FFI callback pins, performs generated
reverse-order field drop during a dedicated admitted turn, then retires
and physically frees the actor heap before issuing `finalized()`.
Foreign completion threads only release pins and enqueue a wake.
Neither pending `get_ready()` nor `get_done()` is treated as a stop
capability, and `get_done()` cannot be fulfilled by stop/close alone.

A generated layout version and FNV64 fingerprint for
`contracts/v1/ACTOR_FIELD_LAYOUT_V1.md` are validated **before**
ActorGroup adoption. The same byte-level contract is verified by the
Java/Graal `NativeActorFieldLayoutV1` class. Mismatched order, names,
mutability or versions fail closed.

**Integration limits:** This native bridge serializes host calls with
a mutex and requires an external CONTROL dispatcher to run
`process_control`. It is not the carrier-releasing production scheduler
or source `spawn`/`await` compiler lowering; `pin_continuation()`
is a host hook for future await lowering. Host callbacks that bypass pin
registration, arbitrary native FFI pointer escapes, process aborts, and
uncaught foreign faults cannot be automatically reclaimed safely.
The worker now retains a shared reference to the canonical group authority,
so a pinned callback may outlive the group's public wrapper without a dangling
ActorGroup pointer. The caller must still keep the CONTROL-dispatch callback
and its external queue alive until all pinned callbacks finish and physical
finalization has been acknowledged. A never-released loan cannot be forcibly
reclaimed without violating memory safety.


## Source-level native continuation + bounded exclusive scheduling (October 10–11)

`ActorExclusiveScheduler.hpp` is a host-owned bounded DATA mailbox with
a reserved, coalesced CONTROL slot. Exactly one logical turn per lane is
submitted to a shared carrier executor; an actor waiting for admission
does not occupy a carrier. The child actor and each supervising parent
require **different** lane instances, so parent CONTROL is not blocked by
a child that is running expensive guest work. Closing the lane rejects
new DATA and invokes cancellation callbacks for admitted-but-undispatched
messages without executing those handlers. A host must provide an
executor that accepts and eventually runs scheduled jobs; the scheduler
does not itself create physical carrier threads.

The LLVM compiler has a **restricted source-level pilot** for a single
actor declaration and a `fnc main(): int` with exactly this sequence:

```ores
const worker = spawn Counter();
await worker.get_ready();
const result: int = await worker.add(20);
worker.stop();
await worker.get_done();
return result;
```

Alternatively `const worker = await spawn Counter();` replaces the
spawn+ready lines. The caller may use exactly one single-i64-argument
actor method, with a pure integer argument and an integer result.
The generated LLVM `ores_actor_Counter_program_step(ctx, state, result)`
is a **six-phase yielding continuation**, returning 0 while awaiting,
1 after physical finalization, and -1 on host failure. Each host
operation is nonblocking/poll-only; host completion callbacks enqueue
scheduler wakes, never call guest code directly. The compiler emits a
host-only cancellation request on terminal error. The native test
harness binds these compiler-generated entrypoints to the canonical
ActorGroup, actor DATA lane and independent parent CONTROL lane, using
three reusable native carriers.

Execution tests cover both source spawn spellings, native result 32,
ready barrier, awaited method delivery, cooperative stop, physical
`get_done()`, and controlled C++ handler panic with error propagation
and physical cleanup. LLVM's negative fixtures reject foreign actor
types and omitted termination barriers. There are separate bounded
mailbox saturation, reserved CONTROL capacity, carrier independence,
logical single-flight and queued-cancellation tests. ASan/UBSan are
separate tests and must be reported only after actually executed.

**Critical scope boundary:** This is a narrow compiler/test-host
integration—not full Java/GraalVM language parity, production async
runtime integration, or a general-purpose source-level typechecker.
The script AST does not yet represent arbitrary actor references,
multiple actors, arbitrary program statements, timers, channels,
cancellable every-kind-of-future, nested awaits, async callbacks,
exception propagation through native C aborts, or source FFI.
`get_done()` currently uses a host future poll rather than a fully
general guest Continuation API. The worker C++ lifecycle bridge
still has its own mutex-backed turn admission inside the mailbox
(the mailbox itself does not block carriers on admission).
No automatic cleanup is promised for C-level process aborts or
externally leaked native pointers.


### Final executed pilot validation

[Funded exact-blob CI run 38113552934](https://github.com/zed-pkg-test/java-app/actions/runs/38113552934):
62/62 Release CTests passed; 100 repetitions of the exclusive-lane
scheduler fixture and 5 repetitions of compiled-source actor panic/cancel
passed; native scheduler ASan/UBSan passed; exact Java/Graal layout smoke
passed; and generated LLVM IR rejected tracing-GC calls. Exact Git
**blobs** are verified; the whole source tree has not been independently
mirrored/certified. This is a draft pilot, not release approval.

A previous sanitizer failure exposed a race in the **test's lifecycle
assumption**, which had finalized a parent scheduler immediately after
a handler resolved an observer promise, before the carrier called its
turn-completed acknowledgment. The regression now waits for real
scheduler-lane release, and the sanitizer check passes. Future actor
runtime implementations must preserve that same distinction.


## Canonical multi-worker scheduler adapter (October 11)

`NativeActorScheduledGroup.hpp` builds on **one**
`NativeActorOwnedHeapGroup` and the canonical
`ActorGroupLifecycle.hpp` ownership registry. Each generated worker
instance has its own persistent heap, field record and bounded
`ActorExclusiveScheduler` DATA lane, while an independent parent CONTROL
lane admits all readiness, result and physical termination callbacks.
The supervisor does not block waiting for a running child or process
guest completions on a child's carrier. Generated i64 method calls
admit on a child lane, and their eventual callbacks are marshalled to
the parent's exclusive CONTROL lane.

Under DATA overload, a call returns a rejected admission result but
still delivers exactly one async failure callback. Stop/cancel closes
the child's DATA lane, cancels its admitted-but-not-running handlers,
requests actor terminal CONTROL, and waits for loans, continuations,
in-flight turns and physically freed heap before resolving `get_done()`.
The group serializes its underlying worker-vector mutation and CONTROL
iteration under a distinct **host-side registry lock**. Actors still
have per-instance logical single-flight execution lanes.

The exact LLVM-generated `Counter` symbols are exercised across
three distinct actor incarnations in
`tests/actor_scheduled_group_generated_host.cpp`: separate mutable
field values, parallel calls, one shared parent CONTROL lane,
normal termination and cancellation while a bounded DATA mailbox
contains queued work. These are generated-method integration tests,
**not yet general compiler lowering for several `spawn` bindings in
one source program**.

The adapter relies on an owned, reliable carrier executor and a
long-lived `shared_ptr` host/controller. The host must not destroy
the scheduler, actor registry or control queue before all terminal
observers settle. Arbitrary FFI pointer escape and C-level abort
remain outside this resource-safety proof.


## Typed actor messaging: only ActorRef.send crosses the mailbox (October 11)

The earlier source-pilot syntax `await worker.add(20)` was removed: a
source-level actor reference cannot invoke arbitrary actor instance methods.
The only approved guest user-message transport in this pilot is
`worker.send(infer struct{by: 20})`, delivered to the actor-local
`receive(ActorMail<{by: int}> mail)` handler. The receive parameter
**declares the accepted payload shape**, and the compiler rejects mismatched
keys, direct `worker.increment(...)` calls, and missing `get_done()`.
The generated native `ores_actor_Counter_receive` entrypoint is a
trusted *mailbox dispatcher* called only from an admitted actor DATA lane,
not a callable guest ActorRef method. The guest is never given the native
entrypoint address.

Example supported typed-message source fixture:
```ores
define actor Counter as
  let count: int = 5;
  receive(ActorMail<{by: int}> mail): void {
    self.count = self.count + mail.value.by;
    self.end();
    return;
  }
end
fnc main(): int {
  const worker = spawn Counter();
  await worker.get_ready();
  worker.send(infer struct{by: 20});
  await worker.get_done();
  return 0;
}
```

This first LLVM fragment supports **one i64 field in one structural
message type**, a one-shot `receive` that invokes `self.end()`, and a
single worker in its compiled `main`. It is not general tagged unions,
multiple typed messages, arbitrary multi-actor source statements, or a
general result channel. The separately tested multi-worker native
scheduler adapter remains available but has not yet been connected to a
general multi-worker typed source compiler. The C++ host callback
implements the post-receive terminal request for the one-shot fixture.

Exact-blob funded CI [38153797830](https://github.com/zed-pkg-test/java-app/actions/runs/38153797830):
66 native CTests passed with no failures, including the positive typed
mail source programs, direct-method rejection and mismatched payload
rejection, original multi-worker CONTROL coverage, native sanitizers,
scheduler/panic repeat gates and the Java layout hash smoke checker.
