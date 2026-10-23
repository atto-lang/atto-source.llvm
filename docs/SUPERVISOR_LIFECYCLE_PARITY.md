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
