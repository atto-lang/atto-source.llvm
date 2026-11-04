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

## Eight fail-closed authority/loan denials

`tests/native_actor_arena_denial_test.cpp` executes eight negative cases in
separate Unix child processes and requires termination by `SIGABRT` rather than
a normal exit or a recoverable value:

1. Wrong actor owner and wrong actor incarnation at turn admission.
2. Foreign actor-domain handle read and cross-actor loan release.
3. Stale slot generation after drop and same-slot reuse.
4. Double release of an already retired persistent loan.
5. Scratch-region handle reused across two actor turns.
6. Attempt to start a turn after the actor arena was retired.

These are eight process-level assertions (the first and second items each
contain two cases). Running children under fork provides independent negative
fault handling; core dumps are disabled. The CTest target
`native_actor_arena_denial_abort_contract` is included on Unix hosts.
Optimized and ASan/UBSan CI runs execute it; the fork/abort suite is deliberately
excluded from TSan, which still runs the concurrent lifetime test.

**Verified local native execution (2026-11-02):**

Original private Git objects were fetched read-only through the GitHub connector
and reconstructed locally; each was checked with `git hash-object` against
GitHub's exact blob ID before compiling. No cross-organization source branch,
checkout token, or credential export was used.

| Exact original source | Verified blob SHA |
| --- | --- |
| `src/main/cpp/oreslang/runtime/ActorArena.c` | `bed8e302d1a37d75e6ca51637d844cbb02311980` |
| `src/main/cpp/oreslang/runtime/ActorArena.h` | `8e788315f23640827b85315a37ba49d27fc548f0` |
| `tests/native_actor_arena_denial_test.cpp` | `0fc6ebf9c5240e1bb0a985ff1c3ef036ea454772` |
| `tests/native_two_actor_arena_lifetime_test.cpp` | `66a42837989dd709fc005735266cb9908bc5091c` |

Actual host-native compilation and execution results:

- GCC optimized (`-O2 -DNDEBUG -Wall -Wextra -Wpedantic -Werror`):
  **8/8 abort-denial cases passed; two-actor 1,000-turn test passed**.
- Clang optimized (same strict flags):
  **8/8 abort-denial cases passed; two-actor test passed**.
- GCC ASan/UBSan (`-O1 -g -DNDEBUG -fsanitize=address,undefined`):
  **8/8 abort-denial cases passed; two-actor test passed**.
- Clang ASan/UBSan (same sanitizer flags):
  **8/8 abort-denial cases passed; two-actor test passed**.
- Positive two-actor sanitizer runs used leak detection; negative abort
  scenarios used `ASAN_OPTIONS=detect_leaks=0:abort_on_error=1`, because
  intentional child-process `SIGABRT` is the passing negative outcome.

These executions used the **real byte-verified ActorArena implementation**,
not a declaration-only stub. They are local host-native results, **not**
GitHub Actions steps, LLVM compiler-generated guest code, TSAN proof, complete
borrow checking, or full zero-GC/hard-real-time certification. The source-org
exact-head native workflow still failed admission with `steps=0` in all jobs
at the preceding code head; separate authorized stepful CI and source-level
compiler tests remain required before merge.

## Concurrent STOP with live carrier and external loan (host-native)

`tests/native_actor_stop_race_test.cpp` models a live carrier executing actor A
while the supervisor initiates STOP. The admitted turn holds a persistent arena
loan, and an independently owned actor B progresses during STOP. The test
proves that a live turn blocks destruction, a loan retained past turn unwind
still blocks destruction, and releasing the last loan permits physical
reclamation. Each test execution repeats the lifecycle 256 times using fresh
actor-incarnation identities and arenas.

Actual local execution against SHA-verified native sources on 2026-11-02:

- `ActorArena.c` blob `bed8e302d1a37d75e6ca51637d844cbb02311980`
- `ActorArena.h` blob `8e788315f23640827b85315a37ba49d27fc548f0`
- `native_actor_stop_race_test.cpp` blob `d4cde9e554873be9d39c674849e01aad63446566`

`git hash-object` independently matched the fetched source objects before
compiling. Results: GCC 14 optimized **PASS** (256 loops), Clang 17 optimized
**PASS** (256), GCC 14 ASan/UBSan **PASS** (256), Clang 17 ASan/UBSan
**PASS** (256), GCC 14 ThreadSanitizer **PASS** (256). A normal native arena
implementation is linked in every run. No credential export or cross-org
source mirror is involved.

CTest target `native_actor_stop_carrier_ffi_loan_quiescence` and the
existing exact-head native matrix were updated to include this test.
However, source-org Actions run
`41395956532` at head `a8a8d204a8539a03b57c4c89864c8111e08c81d5`
reported **zero executed steps in all six jobs** (runner admission failure).

This is **host-native simulation** of an external lifetime loan, not an actual
FFI callback, scheduler cancellation, source actor compilation, LLVM generated
drop-glue proof, complete AOT binary, or whole-program zero-GC proof.
An admitted external callback and exact-head stepful compiler guest run are
still required before any certification.

## Compiled LLVM native two-actor AOT acceptance (legacy scalar subset)

`tests/fixtures/native-aot-two-actor-owned-counter.ores` defines a real
AttoLang/Oreslang source `Worker` actor with one persistent mutable i64 field
and one per-message `owned int` temporary. The LLVM backend must generate
`oreslang_actor_Worker_init` and `oreslang_actor_Worker_on_message`.
`tests/native_aot_two_actor_owned_counter_harness.c` links those *compiler
generated symbols*, not host substitutes, against the native
`ActorOwnedI64Heap.c` implementation. It invokes 1,000 turns per actor, then
stops actor A with an external lifetime-only loan outstanding, verifies that
physical reclamation is deferred, executes nine more turns in actor B, releases
the loan, and finally destroys both heaps.

The `compiled-native-actor-owned-heaps` workflow jobs build the LLVM compiler,
require exact-head checkout, emit native LLVM IR from the source file under
`--experimental-owned-int`, inspect emitted actor/owned temporary symbols,
compile the LLVM IR into a native object, link the real native actor heap and
test harness, and execute the resulting ELF binary in optimized and
ASan/UBSan modes. Compiler, linked object and binary SHA evidence is captured
on a **successful** execution. Any compile, link, verification or runtime
failure stops the job.

A separate `native-aot-unlowered-sealed-actor-state-reject.ores` negative
fixture must be rejected with empty LLVM IR and the diagnostic
`requires sealed handle_state lowering`. This prevents the old scalar actor
implementation from being mislabeled as the newer sealed state contract.

Original submitted Git objects verified:

- AttoLang AOT source fixture blob:
  `1f7534f1ab30639661ab61ba61790d12af4a2a05`
- Native C AOT harness blob:
  `090177cb57dbd1ef456809a83b51053183ec05cd`

The exact C harness file was reconstructed locally and
`git hash-object` matched the GitHub blob. GCC 14 and Clang 17 each accepted
it under C11 with `-Wall -Wextra -Wpedantic -Werror -O2 -DNDEBUG`, using
matching public type/function declarations. That is **syntax-only**, not
compiled AttoLang actor execution.

**Execution state:** the source GitHub Actions run `41397527333` on commit
`002925ea4e66b21595f7eb404d392edf176e3349` reported all eight jobs
with `steps=[]`, including the two new AOT build modes. Therefore **the
compiled guest AOT test is NOT_EXECUTED**; all assertions above describe
test intent, not observed guest behavior. No cross-org private mirror was
performed following an earlier platform safety block on carrier workflow
creation.

The test applies only to the already-supported **legacy synchronous scalar
actor** compilation path. It does **not** prove `state { ... }`, sealed
`critical handle_state`, source lifetime/borrow semantics, async
suspension/cancellation, a general actor-owned region allocator, no host GC,
or hard-real-time safety. These require separately validated compiler and
runtime changes; do not merge or certify from this gate until there is a
stepful exact-source native compilation receipt and independent review.

## Compiler AOT heap-event witness gate (2026-11-03)

The compiled-guest AOT acceptance harness now enables the native i64 heap's
bounded access trace **before** the first actor turn. On successful execution,
it must retrieve a complete snapshot for each actor and reject any overflow
(`summary.dropped != 0`), missing events, cross-actor domains, wrong actor
IDs/incarnations, or a generation mismatch between a temporary's allocation
and corresponding drop.

For each of the 2,009 guest handler invocations, the witness must show one
field read, one owned scalar allocation, one field write and one owned scalar
drop, all bound to the exact admitted actor turn epoch. It also expects each
actor's single persistent field initialization and actor A's additional
externally held lifetime slot. The expected snapshots are 4,002 events for
actor A and 4,037 for actor B, with independent nonzero native heap domains.

The source-hosted CI gate now requires actual **LLVM function definitions**
for `oreslang_actor_Worker_init` and `oreslang_actor_Worker_on_message`,
plus LLVM `call` instructions for scoped-new, scoped-drop, field-read and
field-write. Merely declaring these symbols cannot satisfy the gate. Regex
checks were verified locally against positive function-body and negative
declaration-only synthetic IR samples.

**Local evidence:** the exact updated C harness at Git blob
`c64ad79b91e67451abbbf2122fe6ee700ed5e809` passes strict GCC and
Clang C11 syntax checks (`-Wall -Wextra -Wpedantic -Werror`) against matching
native ABI declarations. The Git hash of the reconstructed source was checked
against the GitHub blob before the checks.

**Not yet executed:** this exact 2,009-turn compiled-source binary,
its heap-event snapshots, ASan/UBSan compiled-source mode, LLVM codegen and
native linking. LLVM development headers / `llvm-config` are not installed in
the local environment. A check that a test harness parses is not a guest
execution receipt. The actual LLVM compiler and native binary must execute
with an independently reviewable exact-source/tree/artifact identity before
any claims of compiled actor parity or zero guest tracing GC.

## Sealed critical scalar actor AOT v0 — LLVM candidate (2026-11-03)

**Default is closed.** This experimental source syntax is admitted only when
`oreslang-llvmc --experimental-sealed-scalar-state` is explicitly provided.
The flag is propagated through the root unit and linked source units, and
the default parser API continues to reject the feature. The flag is rejected
in metadata-only and native hard-real-time compiler modes. Compiled AOT
acceptance runs now require both the positive flag and a negative unflagged
rejection with **no LLVM IR output**.

**Language compatibility caveat:** The normative actor-family design in
`docs/ACTOR_FAMILIES_SEALED_STATE_V1.md` treats `handle_state` as a
runtime-owned, non-overridable state-loan intrinsic rather than a
guest-declared handler method. This **experimental scalar-only ABI** is a
temporary codegen/ownership proof and is **not** canonical source-level
`handle_state(callback)` / lexical `using state.write` implementation.
Promoting the syntax or semantics would require a separate design/spec
reconciliation, compiler-checked capabilities and borrow/loan lowering.
Do not claim language or cross-backend parity from this compatibility slice.


A bounded **candidate**, not the complete cross-backend state-container
contract, is now parsed into `ActorDecl.sealed_scalar_state`:

```atto
define actor Worker as {
  state {
    let total: int = 0;
  }
  pub critical handle_state(int n): int {
    const next: owned int = total + n;
    total = next;
    return total;
  }
}
```

The LLVM frontend accepts only 1..16 initialized scalar `int` fields,
a public non-suspending critical `handle_state(int): int`, at most eight
immutable compiler-owned scalar temporaries when
`--experimental-owned-int` is supplied, and the existing one-turn/one-owner
native heap admission. No object-valued fields, general state containers,
closures, task suspension, async I/O, exception frames, borrow transport,
recursive critical sections, actor concurrency above LOW, or source-level
FFI loans are accepted in this initial slice.

The compiler emits `oreslang_actor_<Actor>_handle_state` for sealed actors
instead of `oreslang_actor_<Actor>_on_message`. Both use the existing
generation-fenced native actor heap ABI and ordered scoped drop epilogue.
Parser validation requires pure scalar initializers, writes and returns
for all sealed handlers, **even without owned locals**, so an effectful call
cannot escape through a critical turn. Top-level guest functions cannot
impersonate generated actor exports. Wasm32 sealed critical lowering remains
a deliberate compile-time rejection until real Wasm guest turn/heap isolation
is implemented.

The exact-head `compiled-native-actor-owned-heaps` workflow is extended to
compile/link/execute both legacy and sealed `.atto` fixtures and demand
different exported LLVM **function definitions**, real native field
read/write + scoped-new/drop calls, and separate 2,009-turn receipts from
the same native actor heap acceptance harness. Negative fixtures require
fail-closed rejection of critical suspension, missing `critical`, legacy
critical claims, effectful state writes/returns, reserved ABI symbol
impersonation, and unlowered Wasm sealed state.

**Executed local native ABI evidence:** source bytes were independently
reconstructed with matching Git blob hashes for the native heap C
(`4f3139bdf65b1a1fab1a37d1515f17d0c032cbfb`), header
(`3a39c6a035f5dbb5955c152ee15ea84658e3bcb2`), and dual-mode
C acceptance harness (`4c97d8a8f762ddab914e9366c6f99c8209613c3d`).
A **test-only C stand-in** for the compiled actor functions linked against
the real production heap and exact harness. GCC and Clang, optimized and
ASan/UBSan, each ran both legacy and sealed ABI spellings successfully:
**8/8 native harness executions passed**, each with 2,009 turns and
4,002/4,037 complete native access events. This demonstrates the native ABI
oracle; it is **NOT an LLVM guest execution receipt** because the handler
was not compiler generated.


**Independent LLVM native-link adapter (not guest codegen):** a locally
hand-authored LLVM IR module with the verified `Worker_init` plus either
`Worker_on_message` or `Worker_handle_state` exports was compiled to a
native object by Clang 17, linked to the same exact production actor heap C
and exact two-actor acceptance harness, and executed. Optimized and
ASan/UBSan runs passed for both ABI spellings: **4/4 LLVM IR adapter
executions PASS**. The adapter explicitly models the compiler's declared
field-init/read/write and scoped-new/drop calls, but the IR is **not** the
output of `oreslang-llvmc`; it cannot certify parsing, type checking,
LLVM code generation or full AttoLang guest parity.

**Still NOT_EXECUTED:** building `oreslang-llvmc` from this branch (local
LLVM dev libraries / `llvm-config` are unavailable), actual `.atto`
parser/typechecker/codegen tests, LLVM-emitted native object/link/guest,
the hosted ASan/UBSan guest, multi-backend conformance and general zero-GC
certification. A GitHub Actions job with `steps=[]` is a runner-admission
failure, never compiler evidence. Preserve draft status and require a
successful authorized exact-head native guest run plus independent review
before promoting this bounded candidate.

### Exact-head guest evidence contract — source org runner required

The compiled-native-actor-owned-heaps job performs two separate native
source-compiler executions, each with 2,009 actor-handler turns, in both
optimized and ASan/UBSan configurations. A successful mode now uploads only
`aot-guest-receipt-<mode>.json` under a run-ID/attempt-specific artifact name.
It contains the source head/tree, Git blob identities, run/job/attempt,
mode, LLVM IR and native object/executable SHA-256 digests, and the two exact
one-line execution markers. It contains **no source code, executables, raw
traces or secrets**.

`tests/emit_sealed_actor_receipt.py` fails closed if the head is wrong,
the tree has changed, a marker is absent/duplicated/polluted, a required
LLVM function is only a declaration, a native ownership call is missing or
commented out, or the legacy and sealed handler definitions are confused.
`tests/test_emit_sealed_actor_receipt.py` runs on the actual GitHub source
checkout before the compiled test and has offline mutation coverage. The
receipt is created and uploaded **only after** the compiler-based positive
runs and negative admission cases have succeeded; no empty-step Actions run
may claim it. Separately, Cortex-M sealed critical state is now explicitly
rejected in frontend admission and LLVM emission because the freestanding
static actor bootstrap lacks the hosted actor-owned heap/turn ABI.

A synthetically constructed local Git checkout may test the receipt
validator's mechanics but is never a guest execution receipt. This
experimental scalar implementation remains distinct from the normative
runtime-owned `handle_state(callback)` state-loan intrinsic. Neither JSON
metadata nor successful native heap unit tests alone prove full zero-GC
coverage or backend parity.
