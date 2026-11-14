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
