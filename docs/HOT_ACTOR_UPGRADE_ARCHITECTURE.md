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
