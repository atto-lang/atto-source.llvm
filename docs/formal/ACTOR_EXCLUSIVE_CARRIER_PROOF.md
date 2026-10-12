# Formal proof target: one physical carrier per actor

An actor may have multiple outstanding suspended logical operations, but at most one physical guest-code executor at any instant:

    forall actor a: count(executing carrier threads for a) <= 1

**This applies to reentrant actors too.** Reentrancy permits interleaving after an await; it never authorizes simultaneous user-code execution for one actor. A serial actor additionally retains exclusive logical *mail-turn* admission across await.

## Formal artifacts

- `docs/formal/ReentrantActorExclusiveCarrier.tla` is the transition system, including atomic actor-wide claim, await/release, wake, completion, cancellation, and stop.
- `docs/formal/ReentrantActorExclusiveCarrier.cfg` is a finite two-actor/two-worker/three-job TLC model; two of the jobs belong to one reentrant actor.
- `scripts/verify_actor_carrier_exclusivity.py` is an independent Python-standard-library exhaustive checker for this model shape. Run it from the repository root; it explores **372** reachable states. The mutation test disables actor-exclusive claim and finds a two-worker overlap counterexample after **65** states.

### Inductive invariant for the abstract protocol

1. Initialization has no worker assignment and no occupied actor lease.
2. The **only** step assigning a worker is an indivisible `Claim`. It requires a free worker and an empty actor-wide lease, and atomically fills exactly those matching slots. Therefore it cannot assign a second worker to an already executing actor.
3. Await and completion atomically remove the current worker and its actor lease; wake, cancel, and stop do not assign any worker.
4. By induction, the property holds for every possible number of abstract scheduler transitions; the bounded checker independently explores the concrete finite instance.

### Important implementation refinement obligations

**This is not yet a proof that all production Java/GraalVM and LLVM paths refine the model.** Before certifying that, audit all execution entrypoints including direct actor receives, queued continuations, HTTP callbacks, actor cleanup, supervisor CONTROL, FFI and child callbacks.

Java: `ActorRuntime.ActorCell.schedule()` CASes `scheduled`; `beginTurn()` checks `activeTurns` under `lifecycleLock`; only `runBatch` should clear scheduling authority after `TurnExecutor` returns and the Truffle context exits.

LLVM: `NativeActorScheduler.c` checks and sets `ActorLane.active` under the scheduler mutex; no callback should run without this exclusive lane and all callbacks must clear it after exiting guest code.

Prove additionally that (a) producer-thread future completions enqueue but never execute guest code inline; (b) no stale generation or cancelled handler reacquires authority; (c) actor-groups and scheduler pools cannot independently mint actor execution leases; (d) control and cleanup paths obey the same rule; (e) release happens strictly after carrier context exit; and (f) no nested FFI callbacks bypass actor admission.

Neither this safety proof nor the enumeration guarantees lock-freedom, fairness, absence of deadlock, timely preemption, or freedom from logical stale-state races. Those are separate properties.

### Running the model

From the repository root:

    python3 scripts/verify_actor_carrier_exclusivity.py

With TLA+ TLC installed (the TLC jar is not vendored here):

    java -cp /path/to/tla2tools.jar tlc2.TLC -config docs/formal/ReentrantActorExclusiveCarrier.cfg docs/formal/ReentrantActorExclusiveCarrier.tla

The Python bounded checker was executed locally and passed. TLC has **not** been executed in this session; do not claim TLC verification until an actual run succeeds.


## Audit/hardening follow-up: every actor-owned code path

"Guest" means the Oreslang code invoked on behalf of an actor. The invariant
has **no exception for reentrant actors**. It covers actor initialization,
messages, methods, source continuations, local failure hooks, actor teardown,
cleanup, and supervisor CONTROL. It is **not** a promise of stable thread
affinity; the same actor may execute its next turn on a different worker.

Concrete defensive runtime checks added to the two companion draft branches:

- **Java/GraalVM:** `ActorRuntime.ActorCell` now stores a temporary
  `carrierOwnerThread` under `lifecycleLock`. `beginTurn` requires the
  owner thread, active physical carrier, and scheduled claim; `endTurn`
  requires the same owner. `runBatch` refuses duplicate carrier claims and
  checks that the owner has quiesced before releasing it. A duplicate
  incorrectly invoked batch cannot clear another carrier's scheduling bit.
  `TurnExecutor` must invoke actor work synchronously on its calling worker.
- **Native LLVM:** the scheduler sets `ActorLane.carrier_owner` while
  the lane is active and validates `pthread_equal` at completion for DATA
  and supervisor CONTROL alike. A TLS `executing_actor` identity allows
  trusted source/native bridge code to reject foreign actor context use.
- **Tests:** the Java actor mailbox boundary test verifies owner authority
  in initialization, receive, async continuation and final cleanup. It
  checks peak actor execution <= 1 without requiring a permanent thread.
  The native DATA/CONTROL test publishes CONTROL for the running actor and
  a separate actor: only the independent actor's CONTROL may overlap
  the original DATA handler. Native CTest registration is included.
- **Review:** actor-group Mailmen, unsafe native callbacks, browser Workers,
  FFI threads and separate process transports still require source-to-model
  refinement tests before this can be called an end-to-end proof.

### Validation status

The standalone bounded Python model was reproduced locally in this audit:
372 reachable safe states; when exclusive claim was deliberately disabled,
the model found simultaneous execution on two workers after 65 states.
The new Java and C production runtime changes have **not** been validated
by executable project CI in this audit: the upstream GitHub Actions jobs
failed without recorded runner steps (logs unavailable). TLC, full Java
Maven/JUnit, real native CTest and sanitizers remain required. Keep the
two PRs draft. No claim of complete source-code refinement is made.
