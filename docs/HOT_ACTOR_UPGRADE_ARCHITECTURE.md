# Oreslang actor/process hot upgrade: execution domains, epoch fences, and backend plan

**Status (2026-10-17): normative design and partial host-side admission primitives; NOT a
production actor hot-reload feature.** Read with the actor physical-boundary,
actor-family, and retirement-barrier documents. In particular, Java main's generic
`HotReloadManager` is a source-generation loader, **not** a mailbox-preserving
actor upgrade. The stacked Java module-contract PR #460 implements more conservative
module loading, but expressly defers actor state migration; it is not merged here.

## Correct memory and process model

An *actor* is not an OS process. Erlang/BEAM lightweight processes have private
managed heaps but ordinarily inhabit **one OS address space**, not one protected
OS address space per Erlang process. Oreslang must distinguish:

| Kind | Ordinary mutable allocations | Shared data | OS address-space protection | Hot code policy |
| --- | --- | --- | --- | --- |
| SHARED | Actor-owned arena/heap, even for siblings | Only explicit frozen regions or capability-governed, synchronized shared regions | Same process is normal; no hostile-code isolation claim | Same-process generation/code-table switch after all actor and shared-region pins reach the barrier |
| PRIVATE (trusted) | Independent actor-owned mutable heap/arena and runtime state | Typed messages, immutable code, explicitly authorized imports | May share the trusted host address space; logical privacy does **not** imply an OS sandbox | Supervisor replaces a checked code generation without sharing mutable actor heap; stable logical ActorRef |
| UNTRUSTED | One owned mutable heap/arena **per actor** inside a separate pooled untrusted execution domain | Serialized/copy-only by default; no host handles or mutable trusted pointers | Java/Graal profile uses **one separately restricted pooled JIT guest isolate for untrusted actors**, not an isolate per actor; enforce a separately validated capability/security boundary | Stage and validate a new untrusted generation within that pool before transferring the routing epoch; never link untrusted native machine code directly into the trusted host |
| PROTECTED WORKER (optional stronger tier) | Independent actor-owned heap/arena | Explicit serialized broker boundary | Separate sandboxed OS process/address space where a deployment expressly requires physical isolation | Verified worker-incarnation handoff and state rebinding; do not silently substitute this tier for ordinary PRIVATE actors |

Here `SHARED` means *permitted to access an explicitly shared data domain*, not
that two actor instances allocate normal mutable objects in a common heap. An
arena is an allocation/lifetime strategy, **not** a security boundary. Multiple
arenas can live in the same OS address space. Native Image isolates, WASM
instances, or an independent process each need their own separately proven
pointer/capability and failure boundaries.

The default trusted PRIVATE actor is **not** one OS process or isolate per actor.
Trusted SHARED and PRIVATE actors both own independent mutable heaps; SHARED
only adds explicitly authorized, synchronized shared-data access. The Java
UNTRUSTED profile uses one pooled JIT guest isolate separate from the trusted
host, while retaining independent actor heaps, quotas and capabilities inside
that pool. A separate sandboxed process is an optional stronger deployment
profile that must prove its own boundary; a Java Context, native arena or
shared JIT isolate alone is not proof of hostile-code containment.

Immutable compiled code units may be shared across actors **only within an
authorized trust/allocation domain and exact code generation**. Shared code
pages and read-only caches must never export private actor pointers or defeat
tenant capability boundaries. Physical protection and code-generation
retirement are separate contracts.

## JIT is not required for hot reload

Hot reload means switching **which executable generation** receives newly admitted
work. It does *not* mean patching instructions under a running frame.

* **Java/GraalVM on JVM:** new Oreslang source/AST in a new checked Context,
  versioned call target, and ordinary Truffle interpretation/JIT where permitted.
  Old call targets/contexts remain pinned. An AOT-compiled Java host cannot be
  assumed to load new Java bytecode, but a supported embedded guest interpreter
  may interpret checked source without guest machine-code JIT.
* **LLVM native:** AOT compile a new `PIC` shared object/relocatable image offline,
  verify manifest, symbols, signatures, platform triple, target features, runtime
  ABI, signatures and provenance, then `dlopen`/`LoadLibrary` through a **trusted**
  loader. Alternatively compile/load with LLVM ORC JIT in a trusted executable
  process. Both paths use a stable C-compatible indirection/vtable boundary; no
  arbitrary cross-version raw function pointers or arbitrary `dlclose`.
* **Wasm:** instantiate a checked new module (AOT-compiled or interpreted),
  migrate *owned* state by canonical wire snapshot, then switch routing handles;
  WASI capabilities must be re-granted for the destination instance.
* **Embedded/no_std:** no OS loader or JIT is assumed. Upgrade requires a
  platform-supplied authenticated image/flash bank or a prelinked function-table
  slot; when unavailable, fail closed or reboot into the new image. Core must not
  depend on stdlib, filesystem, dynamic linking, thread APIs or JIT.
* **iOS/locked-down targets:** do not assume executable-memory JIT or runtime
  native library installation. Source interpretation or approved packaged code is
  deployment-policy-dependent; explicit target capability checks are mandatory.

`AOT + JIT` hybrid is a useful server profile, **not** the definition of
hot reload. For native production rollout, prefer offline signed AOT images and
use ORC JIT when dynamic compilation is actually beneficial and authorized.

## Upgrade transaction and invariants

Logical identity is `ActorId`, not address or PID. Every executable incarnation
has `(ActorId, epoch, imageDigest, schemaDigest, capabilityDigest)`; route tickets,
mail envelopes, callbacks, timers, FFI/GPU leases and shared-region handles carry
or are associated with the incarnation epoch. Compiler-proof receipts must bind
the source tree/dependencies, exported message ABI, state-schema field IDs, 
runtime/stdlib ABI, target/platform features and capability manifest.

1. **Admit/stage:** supervisor verifies signer, immutable artifact digest,
   per-tenant authorization, target ABI/CPU, export contracts, exact capabilities,
   quotas and approved dependency closure. A candidate is not visible to traffic.
2. **Freeze ingress:** atomically stop *new* admission at the actor routing entry;
   persist bounded messages or return explicit backpressure. Independently
   controlled CONTROL cancellation/failure signaling must remain available.
3. **Quiesce:** wait for every executing actor turn (including configured
   multi-thread actor maxima), all suspended continuations, state loans,
   in-flight request lifetimes, outbound reply writers, FFI/GPU DMA, timers,
   pinning shared-memory holders, and any borrowed code-generation handles.
   Deadline expiry aborts/rolls back or uses an explicit restart policy: it
   must not declare quiescence by timeout. A snapshot must not contain raw
   pointers, JVM objects, closure frames, OS descriptors, outstanding loans
   or unowned region offsets.
4. **State compatibility:** v0 supports only **identical** state-schema,
   message ABI, and capability fingerprints. No implicit schema migration;
   `ActorUpgradeEpochGate` (Java host-only) models this fail-closed rule.
   Future versioned `migrate(oldState)->newState` executes in an authorized
   isolated staging domain with deterministic resource/timeout limits and
   an explicit canonical serialized schema validated before publication.
5. **Initialize candidate:** no externally visible side effects before the
   routing commit unless transactional/idempotent compensation is provided.
   Restore owned state in a *fresh* arena/address space; rebind resources by
   supervisor capabilities, never copy raw host handles or closure pointers.
6. **Commit:** compare original actor/route epoch and authorized identity,
   publish new `(image, state, epoch+1)` as **one atomic routing transition**.
   Reject stale/misbound tickets. Drain/preserve old in-flight replies according
   to explicit versioned wire contracts; never deliver one message to two
   incarnations. ActorRef remains stable; incarnation changes.
7. **Retire:** old image/arena/executable pages persist until every generation
   lease is gone, including unwinder/exception paths and native stack frames.
   Destructors run outside global routing locks. Failed FFI reclamation is
   quarantined (do not `dlclose`/unmap until proven safe); actor DONE and code
   image retirement are separate barriers.
8. **Rollback:** before commit, restore old ingress/active generation, destroy
   only safely closable candidate resources and record a reason. After commit,
   rollback is another explicitly versioned upgrade; arbitrary rewinding of
   delivered messages, database writes or network effects is impossible.

Old queued messages must have one explicit policy: preserve with proven
backward-compatible ABI; transform with a verified migration; drain in old epoch;
or reject with a typed, correlated error. A suspended continuation must never
resume in a newly compiled code generation by pointer substitution. All actor
`get_ready()` and `get_done()` lifecycle promises are incarnation-aware and
cannot be accidentally fulfilled by an older worker's late CONTROL message.

## Fail-closed boundaries and remaining work

* **Already present:** Java generic manager stages contexts and admits guest
  capabilities; this audit adds `reloadAndStart` transactional init-before-route
  publication, an optimistic monotonic routing epoch, failure rollback, and
  host-only `ActorUpgradeEpochGate` pin/freeze/ABI-equality model. LLVM provides
  actor-owned native heaps, versioned cells, and retirement primitives but **not**
  a native dynamic actor-image loader. Existing Graal module-contract draft #460
  is not evidence of actor migration or AOT process swapping.
* **Not integrated:** source actor coordinator, true Java actor-wide
  quiescence/epoch ticket instrumentation, protected source-worker bootstrap,
  signed artifact verification, LLVM PIC/ORC loading, serialized state restore,
  distributed handoff, cross-process FD/capability rebinding, native-image
  isolate validation, and live compiler-generated schema receipts.
* **High risk:** current legacy Java `load(...)` still publishes a staged,
  not-yet-started generation for compatibility; **never** use `active()` from
  that legacy path as a serving-ready routing handle. Remove the loophole only
  with a breaking API transition and caller migration.
* **Safety:** dynamically load only trusted, validated native code. A code
  signature proves provenance, not memory safety. For untrusted Oreslang native
  output, use a verified sandbox/process boundary and narrow broker instead
  of unrestricted host `dlopen`. Untrusted code-image sharing must be explicitly
  tenant-authorized and cannot grant ambient host capabilities.


## Follow-up implementation slice: real ingress fence and trusted PIC fixture (2026-10-17)

The Java `ActorRuntime` now has package-private, supervisor-only
`freezeUserIngressForHotUpgrade(ActorRef)` returning an opaque, idempotent
`HotUpgradeIngressFreeze` handle. It checks the actual runtime's two-phase
user-mailbox admission gates (both the initial slot reservation and final
committed-envelope admission) under `lifecycleLock`, rejecting new user messages
with `ActorSealedException`. Previously accepted messages and CONTROL
continuations remain eligible to finish. A bounded `schedulerDrained()`
observation checks execution-carrier ownership, scheduled dispatch, mailbox
accounting under its own lock, source suspension and registered pending
operations; a stale/closed handle cannot lift another handle's seal. The
reservation is released by `close()` on rollback. Runtime-termination
invalidates it.

**Critical limit:** `schedulerDrained()` is an observation, not a durable
quiescence certificate. It does not pin every timer, filesystem callback,
JNI/FFI frame, GPU/OS resource, external broker or shared-region mapping;
a new callback may become eligible after it returns true. Nothing in this slice
changes a source actor's immutable `codeExecutor`, copies its
`ActorOwnedGuestState`, rebinds a stable `ActorRef` to another address space,
or emits a compiler-attested image receipt. The real code+state commit API
must remain unavailable until those requirements are implemented.

The LLVM backend now includes a POSIX **test-only** AOT/PIC experiment.
CTest compiles three independent shared objects (old, new, incompatible) and
a host binary that exercises `dlopen`/`dlsym`, exact ABI descriptor checks,
stale routing epochs, generation-scoped leases, old-version calls after
publication, and no premature `dlclose`. This explicitly proves neither
source-generated LLVM IR nor trusted artifact admission. Arbitrary native
`dlopen` executes initializers **before** metadata inspection; production must
verify the artifact's actual bytes, signature, authorized dependency closure
and execution domain BEFORE the target can run any code. Untrusted actors must
never enter this loader in the supervisor address space.

Next release gates in dependency order:
1. Compiler-private image/state/message/capability receipts with cryptographic
   byte provenance, plus a trusted supervisor admission API.
2. Scheduler-wide epoch pins for outstanding task lifetimes, late timer
   callbacks, FFI and resource leases; `freeze + schedulerDrained` alone is
   insufficient to prove a safe swap.
3. Typed canonical actor-state snapshot/restore and verifier; strict same-schema
   admission first, explicit state migrations only after that.
4. Stable logical actor routing to fresh worker/arena with a single commit epoch,
   old mailbox version handling, and rollback-before-commit.
5. Production native PIC/ORC loader whose authorization boundary executes
   BEFORE code is dynamically linked, real AOT actor sample, ASan/UBSan and
   cross-process sandbox tests.
6. JDK25/Maven, Graal Native Image, LLVM CMake/CTest and exact-head evidence,
   including nonzero Actions job steps and an authorized private source runner.

## Required executable test matrix

1. **Language:** compiler-produced schema/ABI digests; type-safe migration;
   negative contracts, ownership loans, capabilities, no extra imports/exports.
2. **Scheduler:** 1-thread and explicitly configured 3-thread actors;
   waiters, continuations, timers, panic, raised failure, stop/cancel, CONTROL
   during upgrade; thousands of raced freeze/publish/abort/retire operations.
3. **Memory:** shared region lease retained by older incarnation, foreign/stale
   arena handles, FFI active frame, JNI native stack, GPU DMA; sanitizers/heap
   poisoning; bound code-image and arena retention under 100+ upgrade cycles.
4. **Transport:** old/new envelope versions, no duplicated delivery,
   backpressure, reboot/retry, HTTP stream ownership, WS/TCP and proxy routing.
5. **Backends:** exact-head JVM/Graal JIT, Graal Native Image/AOT+hybrid,
   LLVM offline PIC shared library and ORC JIT if enabled, Wasm module instance,
   and embedded dual-image/no-loader rejection. OS-level untrusted isolation
   must be independently demonstrated with real hostile guest programs.
6. **Failure matrix:** worker crash at each stage; invalid signature/import;
   ABI mismatch; failed init; stale/foreign ticket; coordinator crash before
   and after commit; rollback exception; quota/exhausted epoch; power loss;
   misordered late reply; repeated cleanup and double retire.

Only tests executing the **exact checked source tree** and verifying runner
steps/artifact provenance count as backend execution evidence. Source-free
model proofs and host-only Java/C++ tests are useful, but cannot certify
guest actors, native-image execution, protected address spaces or AOT reload.
Never export private source to another GitHub organization without the explicit
reviewed cross-org authorization policy.


## Second audit pass: completion-publication race, canonical checkpoint and router provenance

A production upgrade must never infer quiescence from the absence of a Future
in `pendingOperations` alone. Its completion callback may run after the
Future's removal hook but before the callback enqueues the actor continuation.
**Java's `ActorRuntime.enqueueOnCompletion` now owns a separate bounded
`pendingCompletionPublications` reservation**, created before the Future is
registered and released only after its continuation has been enqueued or its
registration failed. The scheduler drain observation also requires this
reservation count to reach zero. Registration failures release exactly once;
the reservation cannot be exhausted through integer wraparound. A new
real-scheduler JUnit regression stages a pending Future and then completes it
under a frozen ingress gate.

The Java backend also now has `ActorCheckpointEnvelopeV1`, a host-only
canonical envelope for a prevalidated **byte snapshot**:
`ORESCP01` (8 bytes), big-endian u32 version 1, UUID actor identity (16
bytes), big-endian u64 incarnation, four canonical 32-byte SHA-256 digests
(code/state schema/message ABI/capability policy), big-endian u32 payload
length, at most 1 MiB of opaque state bytes, and an unkeyed 32-byte SHA-256
trailer over all preceding bytes. Fixed header is 168 bytes; total length
is exactly 200 + payload length. The decoded record and returned bytes
are defensive copies. Decoder rejects wrong magic/version, foreign actor,
incorrect generation or digests, truncated/trailing data, length overflows,
and checksum modifications. **The checksum is NOT an authentication
signature, and a byte envelope is NOT a serialized guest object graph.**
No executor state is captured, transferred, restored or replaced in this
version; compiler receipts and independent state semantics are still required.

For cross-language conformance, Java JUnit fixes a golden frame digest
`c054268972740370c1be5ff20ceeda21edd17282a69692203acca15b4e6c1793`
for the canonical two-byte sample. An independent, source-free Python model
performs deterministic roundtrips, one-bit corruption and hostile
identity/ABI checks. LLVM CTest registers it in regular and `python -O`
modes, with explicit assertions that remain live under optimization.

The native PIC test router now binds each staged candidate to an opaque
unforgeable per-router authority, *in addition* to its monotonic routing
epoch. A candidate staged by one supervisor cannot be published by another
supervisor merely because their epochs and schemas happen to match. This is
still a **trusted fixture**, not an authenticated artifact loader.

### Evidence for this audit pass

Local checks on exact GitHub blobs:
- Java `ActorCheckpointEnvelopeV1.java` blob
  `885e46f2333d7e89a564db89cb572b5b14076782` compiled using JDK21
  `javac --release 21 -Xlint:all -Werror`. A separately executed
  standalone package-local smoke exercised **2,256** deterministic
  checkpoint assertions and the cross-language golden.
- Source-free Python wire corpus has blob
  `606f9154b8aee9b29fc176ad10b14abc23fa6c24` **identical in Java
  and LLVM repositories**, and the exact local blob passed **1,236 checks
  in each of normal and `python -O` runs**.
- Native PIC test blob
  `54fa36bf9e8b06d0d0bbf2c7e22b0e9c434fe101` and the
  exact four fixture blobs compiled with Clang C++17 warnings-as-errors;
  the test passed once with ASan+UBSan/leak detection and passed in
  **500 additional consecutive optimized executions**. This proves
  host-only PIC generational pinning, stale/foreign candidate rejection
  and safe controlled fixture unloading, *not* LLVM-generated actor code.

The Java ActorRuntime scheduling-race fix and its JUnit integration test
have not been executed through full Maven on the exact PR head. Neither
source repository currently offers usable Actions runner execution; the
private-source cross-org export policy presently authorizes no destination
and must not be bypassed by mirroring a Git tree. Stay in DRAFT.


## Third audit pass: native checkpoint parity and Java resource leases

Both draft branches incorporated latest main via audited two-parent commits.
The Java companion now offers a trusted package-private external callback
lease registered under the actor's lifecycle monitor. While a lease is live,
the supervisor's scheduler-drained observation stays false. A frozen actor
rejects newly registered external work. Duplicate release, stale freeze
tokens, actor shutdown and foreign-runtime access are covered by new JUnit
regressions, which still require exact-head Maven/JDK25 execution.

LLVM now implements the independent C++17 host-only
`ActorCheckpointEnvelopeV1.hpp`. It encodes and decodes the same canonical
big-endian actor UUID, positive signed-int64 incarnation, four digests,
1 MiB-bounded byte payload and SHA-256 trailer as the Java/Python v1 format.
SHA-256 comes from an explicitly injected trusted provider, not from a new
freestanding-core dependency. The optional OpenSSL CTest verifies the same
202-byte golden format and negative identity, corruption and length cases.

Exact native Git blobs `93785beee271551afb44dbd668403e732fd32ae2`
(header) and `3e7233df8ba3e1c49b68315c3a8aed09c59226f9` (CTest
source) passed local Clang17 warnings-as-errors, ASan+UBSan with leak
detection, and 50 consecutive additional optimized executions.

This is a validated host checkpoint **format**, not real Oreslang actor state
serialization, cryptographic source authorization or native executable swap.
External-resource capture is not automatic; all runtime brokers must
participate before any image or arena may be retired. The draft must remain
unmerged until full exact-head integration and independent source-CI
authorization succeed.


## Fourth audit slice: live host counter transaction and rollback (2026-10-17)

This draft now includes a **trusted-host counter actor hot-upgrade proof** in both
backends, built from the real generation gates and the canonical checkpoint
envelope. It is not compiler-produced Oreslang source hot reloading.

**Java**: `ActorCounterUpgradeProof` creates a real `ActorRuntime`
SHARED actor with a stable physical `ActorRef<Long>`, receives actual
mailbox messages, and calls the trusted Java behavior selected by the
host-controlled version. Each executing counter turn holds an
`ActorUpgradeEpochGate.Lease` while committing its
`VersionedActorCell` state. The supervisor's upgrade attempt atomically
fences user ingress, checks the real scheduler's physical quiescence,
prepares an ABI-compatible image, snapshots the one int64 through
`ActorCheckpointEnvelopeV1`, restores into a fresh state cell, publishes
the next generation and swaps the trusted behavior before reopening ingress.
If an old actor carrier is still active, the attempt returns NOT READY without
blocking. Failed restore, incompatible ABI or same-schema state drift abort
before publication and release the ingress fence. Host observations take the
transaction mutex so they cannot read an incarnation after retirement.
`ActorCounterUpgradeProofTest` exercises 42 -> 42 -> 43, stable
`ActorRef` and address, a meaningfully changed v2 behavior, a busy carrier,
incompatible digests, initializer faults and lost-state rejection.

**LLVM**: `NativeCounterUpgradeProof.hpp` composes
`ActorHotUpgradeGate`, `VersionedActorCell` and the portable 1 MiB-bounded
`ActorCheckpointEnvelopeV1`, requiring an injected SHA-256 provider.
The C++17 test `native_counter_hot_upgrade_proof_test.cpp` covers
ABI refusal, failed restore, explicit restore-value drift, preserved state,
changed host behavior, and a concurrent pinned old-generation worker.
Versioned native state observations are synchronized with routing commit.
CTest registers `native_counter_hot_upgrade_transaction_host_only` when
OpenSSL Crypto is available. It does **not** compile or dynamically load
a source-generated LLVM `.ores` code image.

**Reference-model verification**: the identical source-free file
`tests/actor_hot_upgrade_transaction_model.py` in both repos has Git blob
`eaf52579fc0e27deffc994e2601a3e94c5f8c3cf`.
The byte-identical local model ran **96,000 randomized deterministic
interleavings in ordinary Python and another 96,000 under `python -O`**,
including admitted-message conservation, delivery-ticket ordering,
pending-pin refusal, rollback, 23 successful upgrades per run,
and complete no-loss/no-duplicate draining across 32 seeds.
The model's assertions were deliberately replaced with explicit checks
so optimized Python cannot silently disable invariants.
These tests are **model-level**, not evidence that actual Java/GraalVM or
native actor scheduler integration passed.

**Production remaining**: code fingerprints in these narrow examples are
trusted inputs, not signed/compiler-attested executable bytes. A host
Java function and host native function pointer are not a replaceable
compiled Oreslang guest actor. Compiler-generated owned-state schemas,
automatic resource/FFI pinning, protected worker boundaries, rebindable
stable source `ActorRef`, actual AOT/ORC image admission, and full CI
must all pass before releasing a real source actor upgrade.


## Fifth audit slice: real Java Oreslang source-actor same-context upgrade v1

The Java/Graal evaluator now implements the source-actor upgrade contract
directly; this is no longer limited to a fake host counter behavior.

### Runtime transaction

`ActorRuntime.upgradeSourceActorSameSchema(ref, candidateExecutor)` is a
supervisor-only Java host API and requires the runtime ceiling to grant
`HOT_CODE_LOAD`. V1 preserves the existing logical/physical `ActorRef`,
mailbox, output stream, parent/supervision relationship and actor kind.

The transaction:
1. seals new user ingress with the real actor freeze;
2. requires complete physical scheduler drain, including continuation
   publication and registered external-resource pins;
3. captures the old executor/image/state/epoch under `lifecycleLock`;
4. requires old and candidate executors to share one opaque
   `actorUpgradeDomain()` identity, so an evaluator from another
   `OresContext` cannot be spliced into this actor;
5. requires a different code receipt but identical state-schema and message-ABI
   receipts;
6. releases `lifecycleLock` before compiler-owned serialization/restoration,
   so CONTROL/cancellation is never held behind codec work;
7. wraps the serialized state in `ActorCheckpointEnvelopeV1`, bound to actor
   UUID, source-code epoch, code/schema/message receipts and the effective
   runtime actor policy;
8. restores a fresh candidate-owned state object without running field
   initializers or `on_start`;
9. re-snapshots the candidate and requires byte-for-byte state equality;
10. reacquires `lifecycleLock`, rechecks drain and the complete old
    executor/image/state/epoch tuple, and commits the candidate
    image/executor/state as one monitor-protected transition.

A stop, late continuation, changed generation, altered candidate provenance or
other lifecycle change during staging fails closed rather than publishing a
stale checkpoint. This is intentionally an in-place same-`OresContext`
transaction; cross-context/process migration requires a future logical actor
routing/incarnation layer rather than weakening this domain check.

### Real evaluator state transfer

`OresEvalRootNode.Evaluator` now supplies:
- an opaque upgrade domain equal to its owning `OresContext`;
- deterministic structural code, state-schema and message-ABI receipts;
- source actor kind in schema/message receipts;
- compiler-private snapshot and restore of persistent fields;
- candidate restoration into a fresh `OresObject`/`SourceActorState` with
  the original logical actor id;
- no candidate `on_start` execution during restore.

The structural code digest is deliberately independent of source filename, so
renaming an otherwise identical AST does not manufacture a new code version.
It is a runtime structural receipt, **not** a signed artifact digest.

### ORESAS01 scalar payload

`SourceActorCheckpointCodecV1` is the inner deterministic state serializer.
V1 admits only persistent `int`, `bool` and `string` fields. It preserves
field order/name/type, signed int64 exactly, canonical bool 0/1 and strict
UTF-8. Hidden/missing fields, floating-point coercion, oversized integers,
malformed UTF-8, unsupported object graphs and payloads above 1 MiB fail
closed. Raw ActorRefs, closures, capabilities, OS/JNI handles and arbitrary
JVM objects are not serializable.

`ORESAS01` itself is intentionally **not** a checksum. The outer
`ActorCheckpointEnvelopeV1` supplies the SHA-256 corruption trailer and actor
identity/ABI binding. The independent Python oracle was corrected after audit:
structural corruption must fail, while a changed scalar value may remain a
valid but different serialized state.

The identical Python wire oracle blob in Java and LLVM is
`69c52e82cdf8dc589e00e416542bb7f43940d6db`; exact local execution passed
**123 checks in normal Python and 123 under `python -O`**.

### End-to-end Java source test

`SourceActorHotUpgradeExecutionTest` evaluates two genuine Oreslang programs
inside one restricted language context, retrieves their linked real evaluator
executors, and:
- spawns source actor `Counter` v1 with `count = 1`;
- sends 41 and observes 42;
- upgrades to real source v2 with the same actor reference/address;
- proves v2's `on_start { count = 999 }` is NOT rerun;
- sends -1 / -2 and observes 43 / 45 under changed v2 receive semantics;
- rejects identical structural code, changed field schema, changed receive
  message ABI, changed actor kind and a candidate evaluator from a different
  `OresContext`;
- verifies failed candidates leave v1 operational.

The test retries only nonblocking safe-point admission; it never retries guest
messages or guest computations.

These new Maven/Graal integration tests are **committed but not yet executed**
on the exact source head because source-owned Actions jobs continue to receive
zero runner steps.

### Native parity

LLVM now contains `SourceActorCheckpointCodecV1.hpp`, a C++17 implementation
of the same ORESAS01 scalar payload, plus an OpenSSL-backed golden/parity test.
Current exact native blobs:
- codec header: `715a9e70367aef3afebaf4b081b9a70356696fd4`;
- native test: `9133cdc8560ad75ca03ff9c80413407aec844b6c`.

Those exact blobs compiled with Clang C++17 warnings-as-errors and passed the
golden hash, signed-int64 extremes, truncation, trailing-data, malformed UTF-8,
type mismatch, ASan+UBSan/leak detection, and **100 additional optimized
executions**. This establishes state-wire parity only; LLVM still lacks a real
compiled `.ores` actor image/state restore and routing commit.

### Remaining production gates

V1 still does not provide cross-`HotReloadManager` Context migration,
cross-process stable ActorRef routing, arrays/records/options/results,
explicit schema migrations, compiler-derived required-capability/effect
receipts, signed executable/dependency provenance, or automatic generation
leases in every timer/FFI/GPU/shared-memory broker. These remain mandatory
before production hot upgrade.


## Sixth audit slice: cross-OresContext logical actor routing v1

Java now has a **host/supervisor-only cross-context source-actor transaction**
built on top of the same real evaluator snapshot format. This is intentionally
not yet the public/source-level ActorRef implementation.

### Stable logical route and physical incarnations

`LogicalSourceActorRouterV1` owns one stable logical UUID and monotonic logical
incarnation while each generation owns a distinct physical
`ActorRuntime.ActorRef`, ActorId, runtime and OresContext. The logical handle
is the only supported route surface for this v1 coordinator.

A migration uses this order:

1. validate/link an actor-only candidate generation without executing init/main;
2. pin the candidate HotReloadManager generation lifetime;
3. pause host route admission;
4. freeze and physically drain the old source actor;
5. checkpoint old compiler-owned state under the stable logical UUID/incarnation;
6. restore into a fresh physical actor in the candidate OresContext without
   rerunning actor initialization/on_start;
7. await candidate readiness;
8. atomically publish the new logical route/incarnation;
9. stop the now-stale physical actor;
10. keep the old generation pinned until **physical finalization**, then release
    its context lifetime lease and reopen the logical host route.

Before publication, any failure rolls back by destroying the candidate,
releasing the checkpoint freeze and reopening the exact old physical route.
After publication, failures never restore old ingress: the new route remains
authoritative and the old generation stays pinned until later finalization.

### Host admission semantics

V1 deliberately blocks **host** senders at the route monitor while migration is
in progress and holds that monitor through physical mailbox admission. It does
not buffer arbitrary mutable JVM message graphs outside ActorRuntime's normal
transport/copy/freeze path. Actor and scheduler carriers are forbidden from
calling this blocking route surface.

A future guest-visible logical ActorRef must instead use runtime-owned immutable
transport envelopes and continuation-safe nonblocking suspension.

### HotReloadManager generation lifetime

Actor-only generations can be staged with
`stageActorGeneration(...)` and linked with
`Generation.linkActorGeneration(actorType)` using the same internal
LINK_ONLY convention already used by LinkedProgramRunner. Linking does not run
module init or main.

Each logical route holds an `ActorRouteLease` on its generation. Pin admission
is linearized under the owning HotReloadManager registry lock against
`retire(id)`; either retirement wins and the late pin fails, or the route pin
wins and retirement fails. Direct Generation.close(), HotReloadManager.close()
and Generation.start() all fail while a route pins the context. Manager shutdown
therefore cannot invalidate a live actor behind its logical route.

The logical router retains its HotReloadManager strongly and v1 accepts
candidate generations only from that same manager authority domain.

### Runtime/evaluator affinity and restore rollback

Every real OresContext binds its ActorRuntime to that exact source-executor
domain. Raw `spawnCodeActor` and transfer restore reject an evaluator from a
different context/runtime domain.

Fresh restored physical actors can be seeded with compiler-owned checkpoint
bytes. They are marked ready without executing `initializeActor` or
`on_start`. A failed restored spawn now reclaims all resources allocated
before actor registry publication: actor slot, mailbox/continuation envelopes,
output stream/reservations, source state and private memory arena.

Unpublished candidate actors whose teardown has not physically finalized keep
their generation lease. The reaper uses an explicit physical-finalization
predicate, not `ActorRef.isAlive()`; logical stop alone is insufficient to
release executable/context lifetime.

### Explicit v1 topology/output limits

Cross-context v1 is restricted to **root actors only**. Transfer fails if the
physical source actor has a parent, is managed by an ActorGroup, belongs to a
cooperating ActorGroup, or owns actor-created groups. Parent/supervisor/group
lineage is not silently discarded.

Output routing is also not rebound yet. Transfer requires the old physical
output channel to contain neither queued outputs nor registered pending readers.
Unread output or a pending `next()` therefore blocks migration until the
parent consumes/cancels it.

These restrictions are deliberate correctness fences, not claimed production
features.

### Cross-context tests and model evidence

`LogicalSourceActorCrossContextUpgradeTest` stages two genuine Oreslang source
generations in distinct OresContexts/runtimes. Its intended assertions include:

- v1 state reaches 42;
- logical UUID stays stable while logical incarnation advances 1 -> 2;
- physical runtime id and ActorId change;
- candidate on_start/main do not execute during migration;
- restored v2 produces 43/45 and continues after retiring v1 generation;
- state-schema incompatibility rolls back to the exact old route;
- a foreign HotReloadManager candidate is rejected;
- a foreign OresContext executor is rejected at raw source-actor spawn;
- live generation retire/close/manager-close/main execution are rejected while
  a logical route pins the context;
- unread outputs, pending output readers and ActorGroup membership block
  migration rather than being lost;
- injected candidate restore failure restores the old actor and returns the
  target ActorRuntime to its previous actor-count/memory accounting.

`HotReloadGenerationRoutePinRaceTest` races route-pin acquisition against
generation retirement repeatedly and requires exactly one linearization winner.

The identical source-independent route model in Java and LLVM has Git blob
`937cb7c894006894be8cef4f9cbbe0ea69bbf10f`. Exact local executions passed:

- **160,000** deterministic transitions in ordinary Python;
- **160,000** deterministic transitions under `python -O`;
- 44 committed modeled migrations and 18 rollbacks in each run;
- 160,000 active-generation retirement denials;
- host sends injected during modeled route pauses;
- final accepted-message conservation with no lost/duplicate ticket.

The model remains reference evidence only. The new Java/Graal cross-context
JUnit tests have not executed on a repository runner.

### Remaining production gates

The next mandatory work is: a guest-visible/nonblocking stable ActorRef and
transport envelope; parent/ActorGroup/supervision topology rebinding; output
stream rebinding; compiler-derived required-effect/capability/dependency
receipts; aggregate/cyclic persistent state; signed executable provenance;
and a real LLVM compiler-generated actor image using the same route/checkpoint
transaction. Exact-head Maven/Graal/native integration CI remains required
before merge.


## Sixth audit slice: stable logical route across distinct OresContext generations

Java now has a host/supervisor-only cross-context source actor route built on
HotReloadManager generations. This is the first draft path that keeps one
logical actor identity while replacing its physical ActorRuntime/ActorRef and
OresContext.

### Logical identity versus physical route

`LogicalSourceActorRouterV1` owns a stable UUID and logical actor
incarnation. A transparent code hot replacement **does not** increment actor
incarnation. Instead it advances a separate monotonic `routeEpoch`.
Physical ActorRef generation/runtime ids still change and remain stale after
publication. Crash/restart semantics must use incarnation later; code-route
replacement must not be conflated with restart identity.

V1 is deliberately a blocking **host** surface. `LogicalActorRef.send`
linearizes route lookup and physical mailbox admission under the route monitor.
If migration owns the route, host callers wait until rollback or publication.
Actor/scheduler carriers are rejected from this blocking API. No raw mutable
message graph is buffered outside ActorRuntime transport.

### HotReloadManager actor-only staging

`stageActorGeneration` validates source into a fresh restricted Context
without making it the generic active route. `Generation.linkActorGeneration`
uses the repository's existing internal LINK_ONLY control path to link
declarations without module initialization/main. A linked actor generation
cannot later run generic init/main while a logical actor route pins it.

Each live logical route owns an `ActorRouteLease`. Pin acquisition is
linearized under manager -> generation lock order against retire. Therefore:
- `HotReloadManager.retire(id)` cannot close an active actor generation;
- direct `Generation.close()` cannot close a pinned generation;
- manager shutdown refuses while logical actor route pins remain;
- old generation pins are released only after physical actor finalization;
- unpublished candidate actors that fail rollback remain quarantined/pinned
  until their physical finalization can be proved.

### Cross-context transfer transaction

The old runtime's `tryBeginSourceActorTransfer` freezes user ingress, proves
scheduler/resource quiescence, validates that v1 has no parent/supervision
lineage, ActorGroup ownership/membership, unread output, or pending output
reader, snapshots compiler-owned state, and wraps it in the canonical actor
checkpoint bound to stable logical UUID/incarnation and old receipts.

The target runtime claims the transfer exactly once, validates code changed but
state/message/capability receipts remain compatible, restores the checkpoint
into a fresh physical actor owned by the target OresContext, and marks that
physical actor ready **without rerunning on_start**. The logical route is then
published to the candidate. Only after publication is the old physical actor
stopped.

Rollback is fail-closed: old ingress does not reopen until any staged candidate
has physically terminated. After publication, failure of stale-actor stop no
longer releases the old ingress fence in a finally block; stale physical
ingress remains sealed for supervisor recovery. The new logical route remains
authoritative after commit even if old-generation cleanup is delayed.

### Parent-visible outputs

V1 does not rebind output subscriptions. Migration refuses while the old
physical output queue contains values or while the channel has pending reader
registrations. Logical output registration itself occurs while holding the
route monitor, preventing a reader from registering against the old stream
after migration has observed zero registrations.

### Real Java cross-context tests

`LogicalSourceActorCrossContextUpgradeTest` stages genuine Oreslang Counter
v1/v2 source in distinct HotReloadManager OresContexts and checks:
- distinct physical runtime/context and ActorRef identities;
- stable logical UUID + incarnation across transparent replacement;
- route epoch 1 -> 2;
- state 42 restored into v2 without v2 on_start setting 999;
- changed v2 behavior produces 43/45;
- old generation can retire after physical finalization while the logical
  route continues;
- schema incompatibility rolls back to the exact old route;
- unread outputs and pending output readers block migration;
- ActorGroup membership blocks migration until released;
- foreign HotReloadManager candidates are rejected;
- active generation retire/close/manager-close and generic start are denied.

These JUnit/Graal tests are committed but still require an actual exact-head
Maven/JDK21/Graal runner.

### Independent route oracles

Two source-independent models now enforce the route contract with explicit
checks that remain active under `python -O`:

1. `tests/logical_actor_route_upgrade_model.py`,
   exact Git blob `699c010d1402e5702e1a80628241c0dbf93e6df9`.
   Exact local execution PASS in normal Python and PASS under `python -O`:
   **320,000 transitions per mode**, 12 committed upgrades, 21,407 rollbacks
   and 5,090 stale-physical send rejections.

2. `tests/logical_actor_cross_context_route_model.py`,
   exact Git blob `225c7fa36399fb4cc543d8a4d8afc54a102cb130`.
   Exact local execution PASS in normal Python and PASS under `python -O`:
   **160,000 transitions per mode**, 44 commits, 18 rollbacks and 160,000
   active-generation retirement denials.

The second model was corrected during audit to keep logical incarnation stable
and advance a route epoch; older modeling that incremented incarnation for a
transparent code replacement was semantically wrong.

### Remaining production gates

This remains v1 and intentionally excludes:
- guest-visible/nonblocking logical routing;
- parent/child supervision lineage migration;
- ActorGroup ownership/membership migration;
- output-stream rebinding;
- cross-manager/process/network routing;
- compiler-derived required effect/capability receipts;
- aggregate/cyclic actor state beyond the current canonical scalar subset;
- schema migrations;
- authenticated source/native executable + dependency provenance;
- automatic generation leases in every timer/FFI/GPU/shared-memory broker;
- real LLVM-generated .ores actor restoration/publication through the same
  logical route protocol.

Current source-owned Actions still create jobs with zero executed steps, so
those jobs are neither pass evidence nor meaningful code-failure evidence.
Private cross-org source mirroring remains disallowed while the reviewed
destination allowlist is empty.
