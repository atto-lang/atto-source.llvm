# Native supervisor lifecycle parity boundary

This is a host CONTROL reducer, not a scheduler-integrated shutdown tree.
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

CTest registration: `native_supervisor_lifecycle_fsm`.

## Remaining integration gates

1. Freeze the registered tree and fence every worker/restart admission before
   invoking any hook; preserve reverse sibling order and coherent immutable
   tree snapshots. NativeSupervisorRegistry and SupervisorControlTurn are not
   wired to this reducer yet. Its closed admission flag alone does not revoke
   existing scheduler capabilities or suppress on_restart callbacks.
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
