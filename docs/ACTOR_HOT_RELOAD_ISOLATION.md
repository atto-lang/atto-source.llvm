# Actor hot reload and memory isolation — native runtime audit

Status: **runtime substrate only** (2026-10-17). This document does **not** claim
source actor state migration, dynamic linker support, LLVM ORC JIT integration,
or production hot deployment already work.

## Memory and code domains

An actor-owned arena is a **logical ownership boundary**. It is not a process
address space, a hardware protection boundary, or a private garbage collector.
SHARED actors may additionally hold explicitly versioned shared capabilities;
PRIVATE/isoactors own their mutable actor graph but ordinarily still run in one
trusted host address space. A separate process / native isolate is a stronger,
optional placement. Do not create one OS process or Graal isolate per actor by
default. The target Java topology uses one host domain and a single separately
sandboxed UNTRUSTED JIT Engine for many logically isolated untrusted actors.

All actor kinds must support **code-generation replacement** without changing
their public actor identity. Generation labels fence code, frames, callbacks
and remote replies. The actor heap incarnation is a separate identity: code
replacement alone must not reset an actor's arena or release borrows.

## Hot reload without mandatory JIT

- An AOT actor can keep running its compiled version while a supervisor routes
  newly admitted calls to a separately compiled, verified native object/shared
  library. Native symbol/ABI admission and ownership of the loaded library must
  be explicit; arbitrary host FFI is NOT a safe guest upgrade mechanism.
- LLVM ORC JIT can materialize new LLVM IR/object files into versioned
  JITDylibs. Track each code image's ORC ResourceTracker and do not remove it
  until all in-flight code, continuation, unwind, callback and FFI leases drain.
- A mixed AOT-host/JIT-guest mode is legitimate. It does not require converting
  a trusted AOT actor's entire runtime to JIT.
- The selected Graal JVM/Native Image route uses Truffle guest execution
  within the admitted context/isolate. Native Image cannot generally define
  arbitrary new Java classes at runtime. Keep image and sandbox constraints
  distinct from ordinary Java JIT mode.

## Supervisor-owned release transaction (target contract)

1. **Stage**: normalize code-unit identity, check signed/pinned source or object
   digest, target triple, public actor mailbox ABI, capability policy, isolation
   kind, shared-cell ABI, field layout and state-schema version. Do not execute
   guest initialization or change the active route while staging.
2. **Prepare**: reserve bounded code/heap/JIT budget; compile or load the new
   image. Verify relocations/imports and no forbidden pointers to actor arenas.
   For a stateful upgrade, snapshot/serialize private state to an independently
   owned staging arena; do not reuse mutable pointers from an old generation.
3. **Quiesce**: prevent *new* mailbox turns from crossing the old/new boundary.
   An already-running turn retains its old code lease; suspended await frames,
   timers, I/O callbacks and FFI borrowed pointers remain attached to the old
   generation until acknowledged drained or explicitly cancelled. A safe point
   is required for state migration; source callbacks cannot mint their own.
4. **Migrate**: run typed, bounded state migration in a supervisor-owned staging
   sandbox, with explicit old/new schemas. For shared actors, either preserve
   the exact shared-cell layout and synchronization ABI, or use a versioned
   copy-on-write shared region plus a coordinated reader/writer grace period.
   An FFI pointer never crosses versions.
5. **Promote**: after health checks, atomically compare-and-swap the active
   generation against its staged base epoch; a stale candidate must be rejected
   and reclaimed. ActorId/supervisor/mailbox route remain stable, while all
   old wire/control messages carry their generation and authority.
6. **Drain and retire**: pin old code until all in-flight turns, continuations,
   foreign-call users and native return addresses have quiesced. Release native
   code/ORC tracker before or after state memory only according to destructor
   and callback dependencies. Retirement callbacks must not run under runtime
   manager locks; failed candidate cleanup must happen exactly once.
7. **Rollback**: before irreversible migration or external side effects,
   preserve the old route, old memory ownership and old code image. Rollback
   after side effects may require compensating actions or actor restart; never
   claim universal transactional rollback of external I/O.

## Fail-closed default decisions

- Ordinary code-only replacement requires **exact** mailbox/actor ABI, exact
  state schema, identical isolation placement and same or more restrictive
  permitted capabilities. An incompatible change requires a separate explicit
  migration release; it must not silently load.
- Direct same-address mutable references between SHARED and PRIVATE actors,
  between UNTRUSTED and trusted domains, or across a code schema transition
  are forbidden. Serialize/move/copy immutable data or use authenticated,
  generation-fenced shared capabilities.
- A JIT is **optional** for hot reload, not sufficient for safety. AOT/JIT
  code pages remain separate from mutable actor memory.
- A sealed/retired epoch cannot admit new messages or resurrect stale actor
  references. Old handles cannot observe replacement heap state by ABA reuse.
- Preserve zero tracing-GC for native LLVM actor-owned heaps and minimize host
  GC for GraalVM; neither requirement implies a physical address space per
  actor. The pooled UNTRUSTED Graal domain shares a collector by design.
- Keep `core` freestanding: the stage/commit ABI can be independent of OS
  loaders; dlopen, ORC, sandbox policy, file/signature I/O and OTP release
  orchestration belong to optional platform/stdlib layers.

## Current executable evidence and gaps

`NativeSupervisorCodeSlot` pins old revision callbacks, supports epoch CAS
activation, exact ABI equality and bounded retained revisions. The accompanying
native CTest now covers stale and incompatible candidates, live pin caps,
parallel racing promotions and close. This protects **supervisor policy code
revisions** and release cleanup, not source actor state.

Still missing: source actor code-change lowering; universal mailbox identity
and state migration protocol; safe continuation/scheduler freeze; shared-memory
schema migration; actor-owned FFI lease wiring; ORC shared-object loader and
resource tracking; sandboxed UNTRUSTED image JIT E2E; production canary/revert;
cross-backend conformance and adversarial/ASan/UBSan native CI. Do not mark
these complete based on the host-only test.

## Native actor code-only gate (stacked implementation)

The new `NativeActorHotUpgradeGate<CodeImage>` is a **trusted, host-owned
runtime primitive** built on `NativeSupervisorCodeSlot`. It binds a fixed
`ActorHeapIdentity {runtime, actor, epoch}`, actor isolation kind, mailbox ABI,
state schema, shared-cell ABI and capability ABI to the code slot. A turn,
suspended `await` frame or external FFI callback must hold a move-only
`Pin` for its whole lifetime. `freeze(expectedRevision)` closes new DATA
admission, `quiesced(ticket)` rejects promotion until every pin is released,
and `promote(ticket, exactShape, code, cleanup)` performs the revision CAS.
Failed candidate images are reclaimed outside the actor gate/CONTROL mutex;
`abort` leaves the old image active. `close` seals new admission, but existing
pins keep old code alive until the final lease closes.

`CodeImage` is an immutable description supplied by the host. In the current
native CTest it is a string. The native source compiler **does not yet
attach these pins to actual guest call/await/FFI paths**, and the test does
not perform dynamic machine code loading. All three actor kinds are tested as
logical shape/identity categories, not verified physical isolation tiers.

The implementation deliberately **rejects any state-schema/layout change**.
A nontrivial state upgrade still needs separately verified storage snapshots,
typed migration, rollback, shared-cell reader grace periods and a supervisor
health gate. This code-only primitive must never be repurposed to permit guest
raw pointers to cross generations, or to change an untrusted actor's execution
domain simply by changing `Kind`.

Exact mirrored host regression (GCC -O2 and Clang ASan+UBSan):
https://github.com/scintilla-run-test/gleam-runner-conformance/actions/runs/39090397220
The test validates code-only AOT/JIT/AOT generation labels and ownership
bookkeeping. **It does not execute AOT/JIT generated guest machine code.**

## Reclamation ordering / abandoned release safety audit

Native image ownership now starts at the code slot's API boundary, not
only after ABI checks or candidate allocation. For every accepted or rejected
image the host-owned `CodeImage` wrapper must be destroyed **before** the
reclamation callback may unload a shared object or ORC ResourceTracker.
Consequently the runtime requires `CodeImage` to be **nothrow movable and
nothrow destructible**. The callbacks must only schedule trusted host-side
cleanup, never execute guest code. A callback throwing during reclamation is
suppressed by the C++ destructor; actual unload failure reporting and
quarantine still need a runtime-managed error sink before production release.

The supervisor `Ticket` is move-only and has RAII rollback. When an
uncommitted ticket is abandoned by an exception, its destructor reopens
DATA admission, but only if its token still matches the currently frozen
generation. Terminally closed actors and active publication transactions are
never reopened. Releasing a stale ticket cannot undo a later freeze.

Any compilation, ABI encoding, or image staging that may allocate must be
performed **before** transferring code and cleanup ownership to the
underlying immutable code slot. Stage/activate must not assume C++
function argument evaluation order will automatically preserve callback
ownership if an earlier expression throws.

Evidence: exact-source private mirror [run #39091951068](https://github.com/scintilla-run-test/gleam-runner-conformance/actions/runs/39091951068)
covers policy destruction-before-unload, early/invalid admission, throwing
`std::function` copies, abandoned frozen tickets and held continuation pins.
Full executable image teardown, source compiler lowering, and runtime-owned
error quarantine remain explicit release blockers.


## Concurrent terminal close during image publication

Terminal `close()` must never wait for `promote()`: a retired image may
invoke a trusted cleanup callback synchronously from publication and that
callback may request terminal shutdown. Waiting on the publishing thread
would deadlock the same publishing thread. The gate now seals further DATA
and code admission under its mutex, returns without waiting, and leaves the
in-progress publisher responsible for final code-slot closure after its
publish/fail path releases the actor mutex. A concurrent terminal close must
not reopen `frozen` or permit another turn, even if a code revision was
already atomically installed before shutdown.

### Exact-source CI verification (October 18, 2026)

[Private-carrier run #39138131052](https://github.com/scintilla-run-test/gleam-runner-conformance/actions/runs/39138131052)
**passed both GCC C++17 -O2 and Clang C++17 ASan+UBSan** with source Git-blob
SHA verification before compiling. Both jobs executed four binaries:

1. `native_supervisor_code_slot_test.cpp`: copy-failure injection,
   image-wrapper destructor-before-reclaim ordering, and revision leases.
2. `native_actor_hot_upgrade_gate_test.cpp`: admission/freeze,
   quiescence, concurrent turns, abandoned ticket recovery, failure
   during callback copying, and abandonment during active publication.
3. `native_actor_code_execution_test.cpp`: actually invokes three
   statically compiled native function-pointer revisions through generation
   pins (including 1,600 concurrent calls per actor kind).
4. `native_actor_hot_upgrade_reentrancy_test.cpp`: callback-triggered
   terminal `close()` during retirement, and 256 concurrent close/publish
   interleavings. This fourth binary is registered in CTest with timeout.

The workflow archived a manifest and step-outcome report **for each compiler**
as GitHub Actions artifacts. Source PR head at test time includes
`NativeActorHotUpgradeGate.hpp` blob
`7383a51eda7db2e8869e8053379481f932906fd3`,
gate test blob `f8f7caa5bca6c697545dfd132a656d34687b5993`,
static-callable test blob `b48d857df182c8dbf44dd5804a63813e233c3ab7`,
and reentrancy test blob `37eddb2fc1934882ff1d1cd0aded972fb4e7db41`.

The ticket now has `abandon()` for explicit CONTROL release. If the
last ticket goes away while `promote()` is publishing, the publisher owns
deferred freeze cleanup: failure reopens DATA; successful publication
commits normally; terminally closed actors stay closed. Cleanup actions
do not interrupt an active publisher.

**Scope limit:** These are host-only native tests of a trusted code-image
substrate, **not** Oreslang `.ores` compiler lowering or a runtime-bound
`await`/FFI continuation. Static AOT function pointer tests do not load
new PIC code or exercise ORC JIT. Host-only `untrusted` enum coverage is
not a sandbox or a second Graal isolate. Real loader/callback failure
quarantine and full source-repository CI are still required; retain DRAFT.

