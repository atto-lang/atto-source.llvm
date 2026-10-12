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


## Dispatch identity ABA hardening (October 12)

`scripts/verify_actor_dispatch_tickets.py` models three non-reusable
dispatcher tickets and two possible physical workers. The independent local
bounded checker **executed successfully**: 88 reachable safe states and a
five-operation witness showing how an unchecked obsolete release destroys a
newer active ticket after exploring 84 states. An obsolete ticket's release,
cancel or replay must have **no effect** on another ticket, even if the
`scheduled` boolean has cycled false->true. This property is independent
of ordinary one-worker-at-a-time exclusivity.

Java now publishes a fresh `scheduledDispatchToken` atomically with the
schedule claim under `lifecycleLock`; `runBatch(ticket)` must match it to
admit guest code **and to clear scheduling authority**. Shutdown cancellation
invalidates the ticket first. A new JUnit reflection regression replays an
old queued `runBatch` during a newer actor turn; a separate negative test
runs a misbehaving `TurnExecutor` callback off-carrier, which must fail
before invoking any actor constructor or handler.

Native already uses a scheduler-domain/actor-incarnation generation plus a
mutex-protected per-actor active lane, with exact current actor checked by
the callback bridge. An additional DATA/CONTROL test rejects external
producer authority while another worker is actively running the actor.

**Scope:** Both bounded model checkers were actually run locally. These
new Java and native production regressions have not yet been run in full
Maven/CTest/native-image CI; the latest upstream Actions jobs lack executed
runner steps. This branch is not merge-certified.


## October 12 follow-up audit: concrete correctness and executable gates

The Java actor execution boundary regression had a stray literal `''` before a
JUnit assertion; it has been removed. Java also rejects a nested physical
actor context before overriding its ambient ThreadLocal identity, and the
stale-dispatch regression now exercises this hostile entrypoint. This is
separate from the already deployed ticket-identity and TurnExecutor
fail-closed guards.

The native typed supervisor PANIC/DATA regression now uses a monotonic atomic
maximum and explicitly records any simultaneous parent executions, so a
racing callback cannot accidentally overwrite violation evidence. A private
exact-source CI mirror has the updated test blob.

**Local executable evidence (the exact cited repository Git blobs):**

- `VersionedActorCell.hpp`, SHA `02cd3994d0830b6efd1edb0371acc412caabf28f`,
  and `versioned_actor_cell_test.cpp`, SHA
  `0d8334c6c1778343c0ac277c8bebf81b20dce690`, compiled and ran
  successfully with GCC 14 C++17 strict Release and Clang 17 C++17 strict
  Release; Clang ASan+UBSan also passed. Repeated GCC Release 100/100 and
  Clang ASan+UBSan 30/30 with the 32-thread CAS race exercise.
- The exact native `NativeActorScheduler.h` blob
  `93a274141b1d7bfe8d60f91dc37e0953a710d836` and revised
  `native_actor_typed_control_exclusive_test.c` blob
  `56f00b6275534763e1a56c49d2c089509cc55c13`
  passed GCC 14 and Clang 17 strict C11 *syntax-only* compilation.
  This is not a linked or executing native actor scheduler test.
- The exact `verify_actor_dispatch_tickets.py` blob
  `7c7eda09637e3120598fbb2d078b35001813c0fe` reproduced
  88 safe states and its deliberately broken stale-release counterexample.
- Both formal workflows now additionally attempt TLC on the two TLA+
  specifications using the official v1.7.4 `tla2tools.jar` distribution,
  checksum checked against the official published SHA-1. TLC has **not** yet
  executed: hosted project runners continue reporting zero executed steps.

The production Java and C scheduler changes are **not** validated by these
component-level checks. Java Maven/JUnit, native scheduler-linked CTest,
TLC, full LLVM/GraalVM pipelines and source/FFI bridge refinement remain
explicit merge blockers. The optimistic scalar state helper is host-only:
atomic CAS alone never grants a foreign thread authority to execute or
mutate guest-visible actor state outside the actor's admitted lane.


## 2026-10-12 funded exact-source verification (supersedes earlier CI-pending status)

**Machine-checked abstract invariants: PASS.** [TLC run 38219137482](https://github.com/agent-pontifex/agent-pontifex-daemon/actions/runs/38219137482)
ran the exact `ReentrantActorExclusiveCarrier` and `ActorDispatchIdentity`
TLA+/CFG Git blobs from LLVM PR #224 on a private funded runner. Official
TLA+ v1.7.4 (`tla2tools.jar`) SHA-256:
`936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88`.
TLC explored **372 distinct** carrier states (1823 generated) and
**88 distinct** dispatch-ticket states (249 generated), reporting zero invariant
violations. Independent Python model/mutation checks passed.

**Native executable evidence: PASS.** [Funded run 38217714973](https://github.com/agent-pontifex/agent-pontifex-daemon/actions/runs/38217714973)
verified ten Git blobs matching LLVM PR #224, then compiled and ran actual
`NativeActorScheduler.c` with strict GCC C11 Release and Clang ASan+UBSan:
actor scheduler stress, DATA/CONTROL same-actor exclusion, typed supervisor
PANIC exclusion, and injected carrier creation failure. C++17
`VersionedActorCell` concurrency also passed. These are linked runtime
regressions, **not** a complete native LLVM CMake/CTest project build.

**Java executable evidence: PASS.** [Funded run 38219419532](https://github.com/agent-pontifex/agent-pontifex-daemon/actions/runs/38219419532)
verified **266/266** tracked Java source/test/Maven Git blobs against
Java PR #695, then completed focused actor tests and the full GraalVM Maven
suite: **1,471 passed, 0 failed, 0 errors, 0 skipped**.
The negative `TurnExecutor` tests explicitly enqueue initial work because
actor mailboxes are lazy. The older
`ActorCarrierIdentityCleanupTest` was reconciled with the stronger contract:
foreign-worker actor callbacks must fail *before* guest execution and must
not leave a TLS identity on either thread.

**Proof scope remains bounded.** Successful TLC and targeted native/Java
runtime testing establish concrete confidence, but do not yet amount to a
formally verified whole-program Java/LLVM scheduler implementation. Remaining
gates include source-level reentrant actor admission across `await`, full
LLVM compiler/CMake integration, browser/Wasm/FFI ingress refinement, and
independent security review. Do not treat snapshots/CAS as a capability for
producer threads to run actor-owned code.


## 2026-10-12 final integration audit — funded complete project verification

**Java/GraalVM:** Mainline OresVM self-close and structured-cancellation
hardening was merged into the Java actor-exclusivity branch without conflicts.
The verified branch's `src/` + Maven manifest contains **266/266 exact Git
blobs**. The funded
[full Java verification run #38265067414](https://github.com/agent-pontifex/agent-pontifex-daemon/actions/runs/38265067414)
passed both focused actor-execution regressions and the complete Maven suite.

**LLVM/native:** The funded
[full CMake and CTest run #38266195840](https://github.com/agent-pontifex/agent-pontifex-daemon/actions/runs/38266195840)
verified **256/256 source, CMake and test blobs** from LLVM PR #224,
configured/built every original native target, and completed **206/206 CTests,
zero failures**. This includes native typed-PANIC CONTROL, actor DATA/CONTROL
exclusion, lazy carrier failure fencing, actor group tests, logical-race
regressions, and the compiled native sanitizer tests.
The private full-project mirror used these exact compiler/runtime/test Git
blobs; documentation/workflow paths in that mirror were not all pinned, so
do not describe it as a byte-identical copy of every repository document.

**Machine-checked bounded specification:** Official TLC
[run #38219137482](https://github.com/agent-pontifex/agent-pontifex-daemon/actions/runs/38219137482)
completed both TLA+ configurations, finding zero safety violations in
372 distinct physical-carrier states and 88 dispatch-ticket states.

**Explicit outstanding language semantics:** Physical executor exclusivity
does not itself prove *reentrant source actor* interleaving across `await`.
The Java `suspendCurrentSourceReceive` pathway still maintains a single
`sourceReceiveSuspended` and `activeMail` actor-wide state. Actual
multiple-logical-handler interleaving, version-fenced state commits, and
cross-backend compiler-lowered continuation admission must satisfy the
separate acceptance gates in
[Java issue #726](https://github.com/ores-truffle-oreslang/oreslang-source.java/issues/726).
No logical interleaving or production-wide FFI/browser refinement should
be inferred solely from these bounded proofs and passing focused runtime
tests.

**Merge policy:** Physical-lane hardening can be reviewed as a foundation,
but must not be presented as completed guest-language reentrancy. Source
branches remain draft until independent review and their own release policy
permits merging.
