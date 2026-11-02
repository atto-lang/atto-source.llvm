# State-region primitive (experimental)
This is a **standalone native C++20 prototype**, not integrated with LLVM compiler IR,
actor scheduler, or production reclamation. It uses no tracing collector and has
no hidden allocation or synchronization, but it does not manage actual heap arenas.

Build/test:

```sh
c++ -std=c++20 -O2 -Wall -Wextra -Werror -pedantic \
  contracts/state_regions/test_state_region.cpp -o /tmp/ores-state-region
/tmp/ores-state-region
c++ -std=c++20 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
  contracts/state_regions/test_state_region.cpp -o /tmp/ores-state-region-asan
/tmp/ores-state-region-asan
```

The local baseline compile/run was PASS for 100,000 increments with zero counted
C++ operator-new allocations, but the counter does not cover malloc, OS mappings,
or every native allocation API. Assertions must remain enabled in this test.
The same test was also executed locally with AddressSanitizer and UBSan and passed. No GitHub Actions run is claimed.

The prototype is **not thread safe**. All acquire/release/pin/retire state
transitions require external serialization by the actor runtime or lock;
using it directly from concurrent threads is a data race. The primitive
tracks counts rather than individual task identities and does NOT prevent a
wrong task from releasing another task's capability. Both limitations must be
closed with task/capability-identity checks before production admission.

A completed native region backend must additionally enforce non-escaping
borrows at compile time, pin actual callbacks/FFI/continuations, reclaim
owned arenas only after real retirement, and verify mutation allocation +
latency under load. The region struct itself does not free memory.

## 2026-10-30 hardening
The test now uses `CHECK` (which cannot be compiled away by `NDEBUG`) instead of `assert`. Reconstructed exact test logic locally passed optimized, `-DNDEBUG -Werror`, and ASan/UBSan runs. This is developer-host smoke evidence; exact-head GitHub Actions execution has not been established. Do not interpret zero-step workflow failures as test failures or as evidence of exhausted Actions minutes.

Private source export must remain disabled until the source repository's independently reviewed protected-main policy authorizes the exact destination identity and exact source snapshot. The current approved-destinations file contains no destinations. Runner capacity in another org is not transfer authorization. Source-free independent tests can be run elsewhere without copying the private Git tree.


## Task-bound production bridge

The draft now also contains `OresTaskBoundStateRegion.h`. It composes the
existing fixed-capacity `OresStdSyncAdmission` reader/writer gate with the
native execution identity model rather than inventing a second scheduler lock.

Each state-region ticket is bound to:
- logical `runtime_domain + execution`;
- generation-checked execution-store `slot + generation`;
- the exact region object/generation through the underlying scoped ticket.

The preferred binder calls `ores_native_execution_context_store_get` and
verifies that the live generation-checked record carries the expected logical
execution ID. Mismatched IDs, stale handles, foreign tasks, foreign regions,
double release and suspension with a live ticket fail closed.

The runtime gate remains bounded/nonparking: it has a fixed 64-ticket table and
does not allocate per request. ExecutionContextStore itself is separately
allocated runtime infrastructure; this change does not claim that opening that
store is allocation-free. The state-region mutation/admission fast path adds no
new heap allocation.

The compiler CFG now has optional region-backed loan provenance containing
region ID/generation and logical execution/store generation. A region-backed
release must match the exact provenance; a local lexical loan cannot be
laundered into a region loan. This metadata is a compiler safety foundation,
not source syntax lowering.

## CI evidence boundary

Source-org exact-head LLVM workflows continue to fail before runner execution
with zero recorded steps. No private compiler/native CTest pass is claimed for
the new task-bound header or CFG changes.

A separately authored source-free equivalent ran successfully on a real funded
GitHub-hosted runner in `honeypot-r-us-test/hnpt-e2e`:
- branch: `test/state-region-source-free-canary-20261030`
- exact head: `73c3fed5939a1e7158a9c9c625232118c261425e`
- run: `41003990044`
- Python task-identity state-region model: PASS
- optimized C11 `-DNDEBUG -Werror` canary: PASS
- ASan + UBSan canary: PASS

This proves the independent model and alternate runner capacity only. The LLVM
private-CI approved-destination file currently contains no destination entries,
so no private LLVM source was copied to the carrier.

## Actor-resource pin construction ordering

`StateRegionActorLifetimePin` uses the existing `NativeActorRef`
generation/terminal-bit resource accounting. Constructor order is now
**retain actor lifetime first, validate native arena and exact state ticket
second**. Without this ordering, a finalizer could observe zero outstanding
resource loans and physically destroy the arena between an optimistic ticket
lookup and the late resource retain.

If identity, region generation, ticket provenance, or ticket liveness
validation fails, the constructor releases the provisional actor resource
loan before propagating the failure. An already-terminal actor cannot
acquire the loan and fails before reading the arena. The trusted caller must
still keep the arena and region valid until the constructor begins; a raw
pointer passed from already-reclaimed storage is never made safe by a
retention API.

The native regression also exercises a physically `EXECUTING` ticket:
STOP cannot release or suspend that ticket; final actor resource reclamation
remains deferred until exact task end/release and successful pin retirement.
Additional negative tests exercise foreign-generation admission and
post-DONE terminal pin denial. These changes are **authored tests**.
The source-org native build remains unexecuted until a stepful runner is
available; independent local source-free C++17/CAS exercises are not
production compiler or CTest evidence.


## Exact-source local native prototype verification — 2026-10-31

Reconstructed two native prototype source files byte-for-byte from the PR:
- `contracts/state_regions/state_region.hpp` Git blob
  `47ae29c18821884ba3eeeee07469c3ef6371fc5c`
- `contracts/state_regions/test_state_region.cpp` Git blob
  `8cf0a176d5e7a93f80f7395770c2acbf4c17bbb1`

Locally compiled the exact bytes with GCC and Clang, separately under
C++20 `-O2 -Wall -Wextra -Werror -pedantic -DNDEBUG` and
`-O1 -g -Wall -Wextra -Werror -pedantic -fsanitize=address,undefined
-fno-omit-frame-pointer`: **4/4 executions PASS**.
Each exercised 100,000 scalar increments and observed no additional
`operator new` calls in the measured scope. This is an executable proof
of the standalone state-region prototype and its test, **not** the
`StateRegionActorLifetimePin` / `NativeActorRetirementBarrier` integration,
full source compiler lowering, CMake/CTest, FFI provenance, or a hard-real-time
latency/reclamation bound. Those remain draft-only until exact native tests
execute.


## Exact-head native actor-lifetime matrix — 2026-10-31

The new same-repository workflow
`.github/workflows/state-region-actor-lifetime-exact-head.yml`
compiles the **real** `tests/state_region_actor_lifetime_pin_test.cpp` and
`src/main/cpp/oreslang/runtime/ActorArena.c`, alongside their production
actor resource, group lifecycle and Task ticket headers, at the exact PR head.
It runs GCC/Clang × optimized/ASan+UBSan, with `-DNDEBUG` in both modes
to expose Release-only omissions.

To prevent false-green native passes when callers build outside CMake,
the actor lifetime regression now uses always-on `CHECK` for **39
validation sites**, including state ticket operation calls formerly inside
`assert`. It checks unbound-region, forged ticket sequence and already
retired ticket admission, proving that validation failures return their
provisional actor resource loan. Other checks cover executing-ticket STOP
and physical arena teardown only after exact completion.

**Current limitation:** Actions run `41223055980` produced four failed
jobs, all without an assigned runner and all with zero steps. The test is
authored, not executable CI evidence until a runner actually compiles it.
Previous local exact-source standalone `state_region.hpp` prototype tests
are narrower and do not establish the new actor-lifetime integration.


## Concurrent physical retirement receipt — 2026-10-31 (current draft)

`StateRegionActorLifetimePin::release_after_ticket_retirement()` used a
plain `bool active_`, so competing trusted completion observers could both
see the retired ticket and decrement the same `NativeActorRef` resource
loan. NativeActorRef intentionally terminates on underflow; a duplicate
completion must therefore never reach it. The pin now uses an
`std::atomic<bool>` with an `exchange(false, acq_rel)` claim after the
generation-checked ticket has actually retired. Exactly one callback may
call `release_resource_loan()`; all losing observers do nothing.

The existing native arena integration test now races **32 completion
observers** against the same retired ticket and requires one (and only one)
successful actor resource loan release. The exact-head workflow includes a
separate GCC `-fsanitize=thread` matrix leg in addition to GCC/Clang
optimized and ASan+UBSan configurations; these are not mixed together.

**Evidence boundary:** the new concurrent release tests and TSan workflow
are **authored, not executed** on the current private source head. Existing
GCC/Clang local PASS results concern the unchanged standalone scalar
prototype, not this new actor-lifetime code. All source-org native test
jobs still fail prior to runner assignment; the first policy/billing/admission
reason requires an organization administrator, not a compiler fix.


## Current-blob isolated native pin verification — 2026-10-31

Locally reconstructed the exact `StateRegionActorLifetimePin.hpp` blob
`6d31457293157e87df261140885f13f46b19a977` byte-for-byte from
the source repository and compiled it with stubbed `NativeActorRef`,
`OresActorArena`, and task-ticket gate **only in the local test harness**.
The exact, real C++ header was not rewritten or simulated. These stubs are
NOT the private source runtime, so this is **isolated class-level evidence**:

- GCC 14.2: `-O2 -DNDEBUG` and ASan+UBSan **PASS**
- Clang 17: `-O2 -DNDEBUG` and ASan+UBSan **PASS**
- GCC 14.2: `-fsanitize=thread` **PASS**

Each of the five configurations executed 300 pin epochs with 32 competing
completion observers per epoch. The harness required **one** successful
release, zero outstanding actor resource loans, rejection of a previously
retired ticket and rollback on mismatched actor-incarnation admission.
Output: `EXACT_HEADER_STUBBED_RUNTIME_PASS checks=2400 cycles=300
completion_threads_per_cycle=32`.

This **does not** execute the production `ActorGroupLifecycle.hpp` loan
counter, real task-bound gate, native arena destruction or full CMake/CTest.
It cannot certify complete actor-state retirement, hard real-time, or guest
compiler behavior. The actual source-org 5-job matrix remains unassigned
and zero-step.


## Claim-before-probe completion ordering — 2026-10-31

The first concurrent completion hardening still allowed every observer that saw
`active_ == true` to call `ores_state_region_ticket_retired(region_, ticket_)`
before the one-winner atomic exchange. A delayed loser could therefore cross the
resource-loan release boundary and dereference actor-owned region state after the
winner had made physical reclamation legal.

The pin now uses a separate atomic `release_probe_claimed_` lane. Exactly one
observer claims that lane **before** any retirement query. If the ticket is still
live/busy, the claim is reopened while `active_` and the actor resource loan
remain held. If the ticket is retired, that same observer releases the actor
resource loan and only then publishes `active_ = false`. Losing observers never
touch `region_` during that attempt.

The native regression now also races 32 pre-retirement completion observers and
requires zero release receipts, `active()==true`, and one outstanding actor
resource loan before the existing 32-way post-retirement one-winner test.

`active()` is observational rather than dereference authority. A false value is
published only after the actor resource loan has been relinquished, so false means
fully released. A true value is conservative and may include the winner's brief
release-in-progress interval; completion observers are prevented from touching
`region_` in that interval by the separate one-winner
`release_probe_claimed_` fence.

These are authored exact-source tests; private source-org CI still requires a real
assigned runner before they count as native execution evidence.

## Two-actor physical arena regression (native substrate only)

`tests/native_two_actor_arena_lifetime_test.cpp` uses two actual native
`OresActorArena` allocations with different owner/incarnation/domain IDs.
It covers 1,000 alternating, independently admitted turns per actor, persistent
slot preservation, scratch zeroization after turn exit, and lifetime loans
surviving turn exit. A retiring actor must reject physical reclamation while
a loan remains outstanding; the other actor must continue to read its own
unmodified state and retire independently. The final release permits physical
scrub and reclamation of the first arena exactly once.

The test is registered as
`native_two_actor_arena_isolation_and_quiescent_reclamation` in CMake and
included in optimized, ASan/UBSan and TSan source-native CI builds.

**Evidence boundary:** A passing test would validate the native memory
substrate. It does not demonstrate compiler-generated `.atto` actor source,
supervisor scheduling/cancellation, full AOT, absence of all system allocations,
or zero GC in any managed host. These still require an exact-head, stepful
compiled-guest test and a separately audited transitive runtime call graph.
