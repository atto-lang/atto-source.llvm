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
| PRIVATE / ISOLATED (trusted) | Actor-owned heap/arena and runtime state | Typed serialized messages, immutable code, explicitly authorized imports | **Target:** separate protected address space for isolated actors; ordinary Java Context alone does not provide that guarantee | Supervisor can replace a worker incarnation, restoring checked state and stable logical ActorRef |
| UNTRUSTED | Separate heap and independently revocable capabilities | Serialized/copy-only by default; no host handles or mutable shared pointer graph | **Required target:** enforceable sandbox (exec worker + OS sandbox, or separately proven isolate). Native C++ arena alone is not containment | Start candidate in quarantined sandbox, validate capabilities, then transfer routing epoch; never dynamically link untrusted machine code into trusted host |

Here `SHARED` means *permitted to access an explicitly shared data domain*, not
that two actor instances allocate normal mutable objects in a common heap. An
arena is an allocation/lifetime strategy, **not** a security boundary. Multiple
arenas can live in the same OS address space. Native Image isolates, WASM
instances, or an independent process each need their own separately proven
pointer/capability and failure boundaries.

A protected actor worker may be **one actor per address space** when that is the
contract; pooling multiple isolated actors in a single worker without full isolate
teardown changes the contract. Shared code pages and read-only artifact caches may
be reused across domains only with identity/tenant authorization and memory-safe
boundaries. Address-space changes are orthogonal to code-generation changes.

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
