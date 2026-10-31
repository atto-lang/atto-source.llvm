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
