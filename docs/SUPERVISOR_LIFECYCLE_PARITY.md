# Native supervisor lifecycle parity boundary

This is a host CONTROL reducer plus registry/CONTROL admission integration,
not a complete scheduler-integrated shutdown-hook coordinator.
Companion: Java PR #1924, dependency #1911. LLVM base commit:
`b58e43126284a70713a6641f2585edde8a6ca1e5`.

Implemented: deterministic lifecycle transitions, restart reservation/commit,
shutdown winning over reserved restart, hook/child/actor logical barriers,
sticky failure and deadline flags, and separate STOPPED/RECLAIMED phases.
Reclamation requires local physical release plus every frozen child subtree's
release. A clock, cancellation, cleared worker map or logical DONE is never
a physical release receipt.

Each child receipt carries both stable child ID and frozen child incarnation,
in addition to its parent's epoch. Stale parent epochs and stale child
incarnations are inert. Future epochs/incarnations and unknown child IDs reject.
Duplicate receipts leave revision unchanged. Host adapters MUST capture epoch
and incarnation at operation admission; never read the current epoch to label
an old callback at completion. Numeric tokens are not authentication: the host
must also authenticate VM/registry domain and receipt issuer.

Deadline events are inert after logical STOPPED, matching the TLA+ Deadline
guard. Java currently accepts deadlines until RECLAIMED; reconcile this
explicitly before declaring differential parity. Restart requests can reserve
before retirement (like Java); commit requires actor and local physical
retirement. The TLA+ request action is stricter, so a reservation refinement
mapping remains necessary.

State is a trivially-copyable pointer-free bounded record with 64 child tokens
and two receipt bitmaps. No successful reduction allocates, invokes guest code,
waits or creates threads. One owner must serialize reduction/publication on
the existing CONTROL lane. Child lookup is bounded linear scanning; copying
the record is not an O(1)-in-tree-size telemetry design. AoS/SoA/AoSoA/SoAoS
stores may adapt this record using one authoritative physical layout. Do not
maintain synchronized layout copies or put a thread pool on each supervisor.

## Executed focused checks

GCC C++17 strict optimized (-O2 -DNDEBUG) and ASan+UBSan executions passed.
The bounded single-node/two-epoch explorer visits 328 states and 1,764 accepted
changing candidate edges (failure variants can repeat edges). It omits revision
from visited keys and fixes child tokens; focused cases cover stale/future
tokens, duplicates, capacity/bit 63, overflow, early receipts, failed CONTROL,
timeouts and restart fencing. Sibling and depth-three receipt sequences check
descendant release propagation. These are synthetic trusted host receipts.

LeakSanitizer cannot inspect process tasks in this execution environment;
ASan+UBSan passed with ASAN_OPTIONS=detect_leaks=0. No leak-check pass is claimed.
Full repository CMake/CTest, exact pushed-head CI and TLC were not executed.

Reproduce from the source root:

```sh
g++ -std=c++17 -O2 -DNDEBUG -Wall -Wextra -Wpedantic -Werror \
  -I src/main/cpp tests/supervisor_lifecycle_fsm_test.cpp -o /tmp/lifecycle-test
/tmp/lifecycle-test
```

CTest registrations: `native_supervisor_lifecycle_fsm` and
`native_supervisor_lifecycle_registry`.

## Registry and CONTROL integration

NativeSupervisorRegistry now owns one lifecycle record and one CONTROL lane
per supervisor. Hosts route policy turns through `control_lane(ref)`.
`begin_shutdown()` permanently seals registry creation/policy-install admission,
fences managed-worker adoption on every retained live group, and attempts to
fence each CONTROL lane using its existing single-flight lease. It returns false
if any policy turn is active: the coordinator must requeue rather than block a
carrier. A previously admitted restart callback may finish before this fence
commits. After successful commit, new restart decisions escalate without
spending restart budget and ordinary supervisor user turns are suppressed.
Independently constructed CONTROL lanes are not automatically intercepted.

Shutdown preparation freezes exact child epoch tokens in registration order,
then atomically publishes every node's BEFORE_HOOK state under the registry
lock. Allocation failure keeps admission fenced and permits preparation retry;
no hook executes in this API. Child adjacency is prepared once, avoiding a full
registry scan per node. `status()` returns a coherent detached value snapshot,
including distinct admission-fenced and shutdown-committed indicators.

Trusted host hook acknowledgments go through `observe(ref,event)` with the
captured epoch. It checks registered parent/child identity, waits for parent
pre-hook completion before accepting child pre-hook completion, checks actual
child lifecycle before accepting its logical completion, and propagates the
child failure flag. Actor completion additionally checks the authoritative
owner registry, including workers detached by group close/destruction. Closed
groups with still-live actors are not pruned from the registry's accounting.
This managed-actor retirement check does not certify all native loans.

Registry physical-release/child-reclaimed and restart events deliberately reject
until real issuers and restart integration exist. Numeric event data is never a
guest capability. Policy code images already handed out still need explicit
hot-reload/lease integration; sealing policy installation is not policy-image
reclamation.

The new executable test drives the actual registry, group finalizer and CONTROL
lane (no stand-ins), including retained snapshots, depth-three/sibling logical
barriers, stale/future identities, failure propagation, a dropped group's live
worker, timeout truth, 100 coordinated STOP-vs-in-flight-restart races and
100 concurrent spawn-vs-admission-fence races.
Seven focused optimized tests passed: lifecycle FSM, lifecycle registry,
existing actor-group lifecycle, supervisor registry, CONTROL turn, policy code
slot and supervisor hierarchy. New registry tests also passed ASan+UBSan with
the same leak-check limitation above. These are host integration checks, not
compiler-generated source actor execution or full repository CTest.

## Remaining integration gates

1. Route the real OresVM scheduler through registry-owned CONTROL lanes and
   implement asynchronous hook orchestration after begin_shutdown succeeds.
   Preserve reverse sibling order, exactly-once hook admission and failure
   aggregation. The registry currently accepts host hook acknowledgments;
   it does not invoke/await the actual hooks or close groups automatically.
2. Feed authenticated actor finalization, native heap/FFI/continuation loan
   retirement and descendant incarnation receipts from the actual runtime.
   Do not manufacture PhysicalRelease from NativeActorGroup::close().
3. Publish state before admitting hook effects, identify effects by supervisor,
   epoch, phase and revision, and deduplicate admission. Keep bounded deadline
   observers separate from true termination; publish deadline status before
   exposing observer failure. No exactly-once external side-effect claim.
4. Differentially replay Java/native traces, including deliberately stale and
   duplicate callbacks. Reconcile timeout policy and hook failure behavior.
   Run current sibling and depth-three TLC models and establish refinement;
   this C++ exploration is not a TLC run or an arbitrary-tree/liveness proof.
5. Integrate OresVM shutdown, source lowering/checked effects, hot reload,
   fault injection, concurrent STOP-vs-RESTART and retained native loans. Run
   complete native CTest/sanitizers and real Java/Graal/LLVM source parity.

Keep the implementation draft until these runtime boundaries are verified.


## Nested child lifetime integration (draft)

`NativeActorGroup.adopt_child(parent,...)` now commits one same-group,
generation-checked parent edge and child ownership under the group's
admission lock together with the authoritative owner registry. It does not
create another group, mailman, supervisor or scheduler. The same immutable
parent/child edge is used to reject parent physical finalization while a
descendant is outstanding, including after the group has detached its active
worker map. Terminal child finalization retires exactly one parent edge;
stale actor incarnations cannot satisfy a replacement child's join.
`stop_subtree(parent)` atomically fences nested admissions under that
parent and issues each live descendant's stop callback once, outside
CONTROL/owner locks. The new `native_actor_nested_scope_integration` CTest
covers depth-three families, sibling/descendant joins, post-close finalizer
tickets, stale replacement generations, foreign-group rejection and 100
concurrent STOP-vs-nested-spawn races.

**Not integrated:** The standalone `NativeChildScope` reservation prototype
is a host-only reference; it is NOT an additional authoritative lifetime
owner. Production adapters must use the existing `NativeActorGroup` registry
as the single ownership/retirement source, and must not publish actor
references before successful `adopt_child`. A real source-actor scheduler
must call `stop_subtree` on actor terminal exit and retry a rejected parent
physical finalizer after its descendants retire; this glue does not exist
yet. Native FINALIZED still relies on the trusted issuer's proof that the
carrier, continuations, FFI loans and arena are safe to release. Guest
`spawn` lowering, reserved terminal CONTROL delivery, supervisor policies,
cross-supervisor ownership transfer and exhaustive source parity are
separate release gates.

**Evidence:** The CTest target is committed, but exact-head compiler,
CTest, sanitizer and real guest-execution results are pending. GitHub Actions
failure without executed steps is not a test failure or passing result.


### Last-child join wakeup (additional audit hardening)

The owner registry now offers
`register_children_drained(parent, host_enqueue_callback)`: a trusted,
generation-checked one-shot re-finalization hint. It returns `true` if the
parent has no outstanding children; otherwise a bounded single callback is
retained and delivered outside locks when the last child finalizes. Duplicate
subscriptions reject. Both live-group and group-detached child finalizers
trigger the same wakeup. **The callback does not certify local physical
retirement, and does not complete parent DONE.** Trusted actor/carrier
finalization must actually retry, and must retain its obligation if scheduling
the hint fails. The separate arena/continuation/FFI release gate remains.

Test coverage is committed for post-close wakeups, duplicate registration and
already-joined paths. Separate isolated C++17 model testing passed 1,001 cases
with strict warnings and ASan/UBSan; this is **not** exact-head repository CTest.


### SourceActorBootstrap implicit lifetime substrate

`SourceActorBootstrap.c` is the real native C source-actor ABI entered
by compiler-emitted `oreslang_source_actor_spawn`, distinct from the
C++ `NativeActorGroup` host reference substrate. Runtime callback
execution now sets a temporary thread-local *trusted actor identity*
for the duration of the constructor/message only. Actors spawned by
that callback inherit a runtime-owned `lifetime_parent` edge; host
main spawns remain lifetime-root actors. There is no automatic restart,
event-bus group, supervisor creation, per-actor scheduler or carrier.

Trusted `oreslang_source_actor_stop(parent)` seals the entire in-memory
descendant tree under the group admission mutex before issuing stop
to every member in reverse creation/ancestry order. It releases that
mutex before waiting for each native scheduler DONE and before returning
from STOP, then retires and frees every actor-owned heap leaf-first.
An actor attempting to spawn after its owner has crossed the fence
gets a NULL host-ABI result before consuming a slot rather than causing
a process-wide abort; source lowering still owes a checked effect.

`source_actor_implicit_child_scope` CTest now compiles the **actual**
SourceActorBootstrap, NativeOresVM, NativeActorScheduler and actor-owned
heap implementation with strict warnings. Ten depth-three executions
hold a grandchild carrier open while a concurrent trusted host STOP
joins the subtree, and 100 deterministically ordered races prove that
a spawn after a parent STOP fence is rejected without allocating a
descendant. The test-only fence observation API is compiled only under
`ORESLANG_SOURCE_ACTOR_TESTING` and cannot be called from guest Oreslang.

**Outstanding:** This is a limited compiler-runtime ABI bridge,
not generalized `spawn` syntax inside source actor methods, and its
STOP is still a host main barrier rather than a suspendable guest
`get_done()`. C++ `NativeActorGroup` and C `SourceActorBootstrap`
are distinct paths; future integration must not duplicate authoritative
ownership across them. Pending: checked cancellation/spawn rejection
lowering, generation-fenced guest ActorRef lifecycle, source actor
failure/supervisor recovery CONTROL integration, arena/continuation/FFI
loan receipts, and same-revision native CTest/sanitizer execution.
The new test target has been committed, but source-repository Actions
still produce zero recorded job steps. Do not claim this suite passed.

### Checked source-actor send and pointer membership

Source-runtime send, await-ready and STOP validate opaque ActorRef pointer membership while holding the same group mutex that guards actor reclamation. The new `oreslang_source_actor_try_send` returns false for missing/stale/stopping handles without aborting. The legacy void guest ABI keeps its existing fail-closed contract pending checked-effect lowering. This pointer-membership fix does not address allocator-address reuse (ABA); full source handles still require incarnation fencing.

The `source_actor_opaque_ref_validation` CTest covers live checked admission, invalid/stale handles and retired pointers while another actor keeps the VM active or after complete VM shutdown. Negative legacy-ABI tests use fresh exec'd processes, not forked copies of an active multithreaded scheduler. The subtree STOP race fixture also checks that a later try_send rejects admission. Test targets are committed but exact-head CTest is not yet executed.

### Monotonic source handles and overlapping STOP (follow-on)

The native C `SourceActorBootstrap` no longer publishes malloc addresses
as source ActorRef values. Its restricted `void*` ABI represents a monotonic,
process-unique, nonzero actor ID; admission compares that ID against currently
live group registry records while holding the group mutex. The root
`await_ready` continuation receives the same token encoding. This prevents
a freed allocation address being recycled into authority for an earlier
source ActorRef. The token is **not** a sandbox-secret capability; untrusted
source code must not be permitted to forge/cast arbitrary native pointers,
and a portable typed `ActorRef` value format remains a future gate.

A checked trusted-host `oreslang_source_actor_try_stop` refuses a missing
or already-stopping actor, overlapping parent/child STOP claims, and
carrier-context blocking. It also refuses to reclaim an actor pinned by a
pending ready continuation. A successfully admitted STOP still seals the
whole subtree atomically under the group lock, waits for scheduler DONE
outside the lock, and physically retires actor-owned heaps leaf-first.
The legacy `stop` maintains the original fail-closed single-owner behavior.

Committed regressions cover 32 source ActorRef retirement/reuse cycles,
concurrent parent/descendant STOP denial while a live grandchild carrier
holds physical completion, 10 child-first STOP / parent retry cases,
32 concurrently stopped independent roots and 100 spawn-vs-STOP races.
These tests are **committed/registered, not reported as passing** until the
exact private LLVM PR tree is actually built and run under CTest/sanitizers.


### Native host STOP dispatch recovery and turn-admission fence

The C++ `NativeActorGroup` CONTROL-plane substrate now rejects *new*
ordinary DATA and CONTROL handler admission after its worker enters
`Phase::stopping`; an already reserved CONTROL envelope remains drainable
on the actor's exclusive lane. A stopped actor can never admit additional
guest work merely because its physical finalizer has not yet completed.

A throwing trusted STOP enqueue callback no longer counts as delivery.
The actor's generation-specific shared ticket records
`stop_delivery_failed()`, clears the dispatch gate and retains the same
underlying mutable, idempotent host STOP callback after manager detachment.
A trusted scheduler can invoke `NativeActorGroup::retry_failed_stop(ref)`
even after group destruction. Retrying a STOP never acknowledges physical
DONE or calls guest code; true completion still requires the generation-
checked physical finalizer. Existing `stop_workers`, `stop_subtree` and
`close` can reissue a callback whose preceding delivery was unconfirmed.
All copies of the callback dispatch wrapper share the same callable object;
a mutable callback does not restart from its pre-failure captured state.

**Required host contract:** STOP callbacks must be idempotent and signal
unconfirmed admission by throwing. They must not throw after confirming
delivery. Callback captures must remain lifetime-safe until delivery or
physical terminal acknowledgment, and the trusted scheduler must retain
failed STOP retry obligations after close. An exception swallowed by a
callback itself cannot be diagnosed by the native group.

New regression cases cover transient STOP rejection in the live manager,
a child failure followed by full-subtree retry, a detached manager retry,
and a mutable callback whose state must survive manager destruction.
The actor-group lifecycle suite also checks STOP rejects fresh DATA/CONTROL
admissions but drains CONTROL reserved before the fence.

**Verification status:** these are committed source/test changes, not an
exact-head CTest or sanitizers pass; source-organization Actions still
report zero-step failures. An independent source-free C++17 model of the
retry ticket ran 2,000 trials with four concurrent retry contenders and
passed optimized GCC plus Clang ASan/UBSan. That model is not production
source execution. The review-only private carrier [Java draft #1959]
(https://github.com/ores-truffle-oreslang/oreslang-source.java/pull/1959)
does not authorize cross-organization transfers; issue #962 remains the
security gate.

### Retryable last-child wakeup and parent completion fence

The native C++ group now retains failed enqueue-only parent wakeups after
the last child physically retires. The original mutable callback is stored
on the parent incarnation ticket, including after group destruction.
A trusted scheduler can call
`NativeActorGroup::retry_failed_children_drained(parent)`; the parent ref
exposes `child_join_notification_failed()` for observability.

Child-owner retirement sets a parent-ticket pending flag **before**
releasing the registry lock. Parent `finalized` and detached
`RuntimeFinalizer::finalized` fail closed until the callback successfully
enqueues. Subsequent observer registration cannot replace a failed/in-flight
notification. An enqueue receipt does not itself publish parent DONE or
guarantee native loan/arena reclamation.

The callback MUST be idempotent, enqueue only (never guest code or inline
parent finalization), have safe captures across group close, and throw
only if delivery was not confirmed. This is a narrow C++ host boundary,
not source-level ActorRef Future lowering. New native nested-scope tests
exercise two transient failures, new child admission and observer rearming,
parent DONE rejection, and retry after manager destruction.

Exact-head production CTest and sanitizers remain **unverified** because
source-org GitHub Actions jobs complete with zero recorded steps. The
source-free private runner is not authorized to receive source without
independent approval under Java security issue #962.

### Exact-revision local production test entrypoints

Runner allocation/billing/organization policy is currently preventing
source-organization GitHub Actions jobs from starting. A failed zero-step
Actions run is not evidence that any compiler or runtime test passed or
failed. The protected private-CI destination allowlist is still empty;
do NOT upload private repository source to another organization or interpret
a source-free runner canary as source-transfer authorization.

Two source-local, no-network, no-credentials verification scripts now
support exact-head testing from an existing authorized checkout:

```bash
# Freeze the SHA from the review/PR, not from an unreviewed moving branch.
EXPECTED_COMMIT='<40-hex-commit-from-LLVM-PR-864>'
bash scripts/verify-actor-lifecycle-direct.sh "$EXPECTED_COMMIT"
# If LLVM + CMake packages are installed, also run actual CTest targets:
bash scripts/verify-actor-lifecycle-local.sh "$EXPECTED_COMMIT"
```

Both scripts reject a mismatched Git HEAD and a dirty worktree and record
the actual HEAD and Git-tree SHA in the out-of-tree build receipt. By
default they repeat targeted lifecycle tests three times and compile a
separate Clang AddressSanitizer + UndefinedBehaviorSanitizer configuration;
the repetition count can be set with `ORES_LIFECYCLE_REPEATS` (1..100).
Use `ORES_LIFECYCLE_ASAN=0` only to explicitly skip sanitizers.
Each matrix prints a pass marker **only** after successful execution.

The direct test script builds the exact production C runtime source objects
and production C++ header tests, not models or mocks. It does not require
LLVM or CMake, and covers these exact CTest-equivalent programs:

- `source_actor_implicit_child_scope`
- `source_actor_opaque_ref_validation`
- `native_actor_nested_scope_integration`
- `native_actor_group_managed_worker_readiness`

The CMake script builds their exact CMake targets, asserts all four test
names are registered using `ctest --show-only=json-v1`, then executes
their real CTests. Note: `actor_group_lifecycle` is **not** the registered
CTest name; the actual name is
`native_actor_group_managed_worker_readiness`.

The direct shell script was verified locally at its exact fetched Git blob
(`b3e7238ec8c28f36bf6a256921588aae38bd0280`): `bash -n` passed,
and isolated Git-fixture tests confirmed wrong-HEAD and dirty-worktree
inputs fail closed with exit code 2. **The OresVM production source was not
available in that local execution environment**; therefore compilation,
CTest and sanitizer results for the LLVM PR remain unverified.
