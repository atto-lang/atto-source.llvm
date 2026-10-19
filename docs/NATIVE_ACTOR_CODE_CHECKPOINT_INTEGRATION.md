# Native code-only actor upgrade: single-gate checkpoint integration

Status: **host-only checked C++17 proof**. This is a staged
integration on LLVM #602 (which is stacked on #595), using the exact
`ActorCheckpointEnvelopeV1.hpp` blob shared with the separate epoch/transaction
draft #597. It is not a production source-level actor reload.

## One authority; no split publication

`NativeActorCodeCheckpointCoordinator` has exactly **one admission and
publication authority**: `NativeActorHotUpgradeGate<Image>`. The immutable
`Image` holds a statically compiled `int64(int64,int64)` code callback and
its checkpoint metadata together. Each `Gate::Pin` therefore protects the
executable generation **and** the manifest returned to the turn. The state
cell remains actor-owned and independent from code generation.

A separate epoch gate is deliberately **not** nested inside this code gate:
two independent `freeze` and `publish` authorities could disagree about
quiescence or leave new code paired with old schema metadata.

```
Trusted native code-only transaction
  Pin current generation -> copy exact code+manifest -> release Pin
      |
  Gate.freeze(expected_revision) ---- admission sealed for new DATA
      |
  Gate.quiesced(ticket) --- NO -> abort freeze; release staged candidate
      |
  Snapshot one actor-owned scalar via VersionedActorCell
      |
  Encode/decode canonical v1 checkpoint (injected real SHA-256)
      |
  Validate same ActorId/incarnation/state/message/capability fingerprints
      |
  Run trusted restore probe outside CONTROL mutex
      |
  Reject any mutated restored value (NO state migration)
      |
  Gate.promote(ticket, Image{new_code, new_manifest}, cleanup)
      |
  Atomic publication; old image stays pinned until final old invocation
```

**Failure ownership**: a staged candidate owns one reclamation callback.
Any validation failure, stale ticket, outstanding old turn, hash/restore fault,
or race must reclaim that candidate once, without reclaiming live old code.
Old DATA admission is restored by ticket RAII on abort. The code slot owns
cleanup after successful publication and runs final code-image reclamation
outside CONTROL locks.

**Terminal close** seals actor admission permanently. A reclaim callback that
reenters `close()` cannot wait for the publisher or reopen DATA. Last code
pins protect old executable generations through accepted callbacks and native
exception unwinding.

## Trust/ABI constraints

* The digest in the checkpoint wire is **unkeyed SHA-256**; it detects
  accidental or adversarial modifications only relative to a separately
  trusted expected digest. It is not an artifact signature, authentication,
  author authorization, or proof of memory safety.
* Manifest digests in this host fixture are trusted inputs. The actual
  `.ores` compiler must derive and attest source/dependency/code digests,
  ABI field IDs, target features, capability policy and code ownership.
* Statically linked C++ function pointers are used for executable dispatch.
  The trusted fixture PIC `dlopen` test in #597 is **separate**. Neither
  test authenticates a native image before loading, and `dlopen` can run
  constructors before symbol/ABI inspection.
* Actor heaps and scalars remain separately owned. Trusted PRIVATE does not
  mean one OS isolate per actor; SHARED only permits explicitly authorized
  immutable/synchronized sharing. Untrusted Java actors target one distinct
  pooled guest JIT isolate, retaining one independent actor heap each.
  Native heap arenas and enum tags do not provide hostile-code containment.
* Concurrent DATA turns on the host scalar use a versioned CAS; a conflict
  fails closed. The actual serial actor scheduler must enforce its own
  single-flight handler semantics. This proof neither processes Oreslang
  mailboxes nor implements actor-state migration.
* Per-generation leases from compiler-generated `await`, callbacks,
  timers, replies, FFI, GPU DMA and shared-cell loans must be counted
  before production quiescence is trusted.

## Verified evidence (October 18, 2026)

Source branch: `integration/native-actor-code-checkpoint-20261018`
Draft PR: #627, base #602.

Successful GitHub Actions private carrier:
https://github.com/scintilla-run-test/gleam-runner-conformance/actions/runs/39142600461

* GCC C++17 `-O2 -UNDEBUG -Wall -Wextra -Wpedantic -Werror`: PASS.
* Clang C++17 `-O1 -g -UNDEBUG -fsanitize=address,undefined`: PASS.
* Exact Git blob identities verified **before** compilation for the
  coordinator, test, checkpoint v1, generation gate, code slot, actor heap
  identity and scalar cell.
* 2 uploaded immutable-for-retention-window test evidence artifacts, with
  compiler settings and exact source Git blob manifests.
* Tests: source callback v1->v2 with changed behavior and unmodified state;
  foreign/code digest, ABI/capability and stale-revision rejection; failed
  initializer; pinned old turn preventing quiescent upgrade; retry after old
  callback completes; reentrant code-image unload shutdown; malformed initial
  actor image, wrong actor identity, invalid incarnation and null SHA provider;
  restore callback refusing new DATA admission during freeze.

Related independent source-blob CI:
- #602 code slot, actor gate, callable generations, reclaim-to-close:
  https://github.com/scintilla-run-test/gleam-runner-conformance/actions/runs/39138131052
- #597 epoch/checkpoint, trusted test-only PIC shared-library swap, actor
  transaction model:
  https://github.com/scintilla-run-test/gleam-runner-conformance/actions/runs/39138522192

## Production acceptance still open

LLVM #606: source-generated actor turns, `await`/FFI/cancel/exception pin
lowering and real source `.ores` v1->v2 execution with stable ActorRef/mailbox.

LLVM #607: **authenticated before execution** AOT/ORC image admission,
compatible ABI manifest binding, relocations and safe image retirement.

LLVM #610: supervisor-owned durable quarantine and observable cleanup errors,
including retry and bounded storage, not silent exception suppression.

Common-source #73: Java/LLVM/Wasm conformance and correct trust-domain
isolation. Actual repository-wide CMake, ARM64, ASan/TSan where supported,
interpreter/guest execution and long-running orchestration remain necessary.

**Do not mark PR #627 or its parent PRs ready for production merge based
solely on host-only green carrier tests.**

## Continuation-lifetime contract — verified additional pass

`SuspendedCodeLease` is move-only, owns one existing gate `Pin`, and
carries **only** immutable code and its version-bound manifest. The lease may
move to another native worker, survive coordinator destruction while the
underlying immutable code is retained, and be released from an FFI/await
completion path. It must not grant actor-owned state access or count a
suspended frame as quiesced. The real guest scheduler still needs to check
actor identity/capability and continuation validity before resuming a turn.

Exact source-blob, two-compiler CI
https://github.com/scintilla-run-test/gleam-runner-conformance/actions/runs/39145223446
passed GCC C++17 -O2 and Clang ASan+UBSan. Each job ran the complete
integration executable **10 successive times**, including 128 concurrent
snapshot/publication and 128 close/snapshot race iterations per execution;
thus each compiler exercised 1,280 of each race. Both jobs uploaded
immutable-for-retention-window blob manifests and step-outcome artifacts.

## Next actual native source actor integration: ingress is the linearization point

Inspected existing `SourceActorBootstrap.c` and
`NativeActorScheduler.c`. `SourceActorBootstrap::deliver` already enters the
actor-owned native heap and invokes a compiler-emitted `on_message`
function pointer; the compiler therefore has a real C ABI handler entry.
But a successful `ores_native_actor_scheduler_try_send` happens **before**
`deliver`, under the scheduler mailbox lock. Treating a pin acquired only
inside `deliver` as the admission fence would be INCORRECT: older accepted
messages could be queued during a freeze and then blocked or executed by the
wrong image.

Required next implementation slice for LLVM #606:

1. Under the scheduler's real mailbox lock, atomically mark the actor's DATA
   admission closed and capture the exact scheduler lane incarnation. CONTROL
   cancellation/failure and teardown must remain available. Reject newly
   attempted DATA sends with a typed retriable frozen/backpressure result;
   do not acknowledge a rejected message as accepted.
2. Pick and enforce an old-mailbox policy. Initial v1 policy: drain all
   previously ACKNOWLEDGED queued turns in the old code generation. Those
   messages must continue to dispatch despite new ingress being frozen; do
   **not** re-route them to the replacement image.
3. After `queued + active == 0` for that actor, independently verify all
   compiler-rooted suspended `await`, FFI, callback, timer and outstanding
   immutable-code leases are zero. Scheduler physical idleness alone is not
   generation quiescence.
4. Freeze the SINGLE code gate, validate stable ActorRef/ActorId,
   actor-owned heap, old snapshot and identical schema/message/shared-cell/
   capability fingerprints. Publish code+manifest atomically, then reopen
   scheduler DATA ingress in the same ownership/epoch protocol.
5. Failure before publication: restore old admission only when the original
   incarnation is still live, keep already-accepted FIFO messages owned by
   old generation, and reclaim only the rejected staged code.
6. Shutdown or source `stop` racing the transaction must remain terminal.
   No stale gate ticket or old scheduler lane can reopen a newer incarnation.

No scheduler code or compiler-generated await lowering is changed by this
host-only PR. This is a *documented release blocker*, not a passing test.
Source fixtures `tests/fixtures/source-actor-fifo-ten.ores` and
`source-actor-spawn-stop.ores` provide existing real compiled actor
dispatch tests to extend; `source-actor-spawn-unavailable-await.ores`
explicitly documents missing guest await support. The first source
end-to-end must verify old FIFO drain, stable ActorRef and unchanged actor
heap, then v2 dispatch, including a forced rollback and concurrent `stop`.

## New source-generated AOT/PIC executable-image integration (2026-10-18)

The same PR now includes `tests/source_actor_pic_hot_reload.py`, two
independently compiled `.ores` `Counter` generations, and
`tests/native_source_actor_pic_hot_reload_test.cpp`. Unlike the preexisting
host function-pointer tests, this test compiles the **actual Oreslang actor**
twice through `oreslang-llvmc`, materializes two ELF PIC shared objects,
resolves `oreslang_actor_Counter_init/on_message` from each version, and
calls both generations against **one actual `OresActorI64Heap`**. The
`NativeActorHotUpgradeGate` pins old code, freezes DATA admission, rejects
a deliberately incompatible state ABI, publishes only at quiescence, and
retains original actor heap/identity as behavior changes from 42 (v1)
to 86, 128 (v2). Old DSOs remain pinned until release.

Registered Linux CTests:
- `native_source_actor_aot_pic_hot_reload`
- `native_source_actor_aot_pic_hot_reload_asan_ubsan`

**Verification status: NOT EXECUTED on the exact source branch.** Source-org
workflow jobs still fail *before runner steps*, including the exact-head
LLVM compiler/native execution workflow. This is new code awaiting real
compilation, not an achieved AOT hot-reload certification. Its compiler
syntax, linker model, two native image loaders, byte-precise ABI and
sanitizer behavior must be confirmed by an actual executing runner.

**Trust boundary:** These DSOs are compiled from fixed, trusted test source
into a private temporary directory. This is intentionally not an
externally accessible loader: the `dlopen` host fixture does **not**
independently verify artifact signatures, dependency trust, constructors,
platform ABI, relocations or a secure load path. Passing the test will
not authorize loading an attacker-controlled image. The in-test
synchronous `dlclose` callback is also not an accepted production cleanup
or durable quarantine mechanism; see LLVM #607 and #610. No LLVM ORC
execution, guest `await` generation pin, scheduler mailbox freeze, user
`ActorRef` continuity, multi-threaded DATA execution or state-schema
migration is claimed here. The runtime-owned scheduler linearization
described above remains mandatory.

## AOT/PIC vertical-slice audit hardening (2026-10-18)

The source-PIC fixture now verifies its two actor field declarations with
a restrictive, fail-closed, independently parsed TEST layout oracle before
invoking any compiler or loader. The oracle requires the exact ordered
`const base: int`, `val delta: int`, `let total: int` fields, and refuses
field renames, type/mutability drift, extra/reordered fields and unexpected
handler syntax. Eight source-only selftests are registered both ordinarily
and with Python `-O`; a malformed source cannot silently use an arbitrary
hard-coded C++ `Shape` as evidence of compatibility. This is a limited
fixture check, **not** a general compiler-authenticated ABI manifest, which
remains required for production under #607.

The native fixture also:
- Pins the initial code image before executing compiler-generated `init`.
- Simulates a live suspended code pin, deliberately attempts publication
  while pinned, and requires rejection/reclamation of the rejected image
  without unloading the active code.
- Rejects foreign actor heap identity and post-close code leases.
- Checks code-only heap continuity and final DSO/heap reclamation.

**Execution status:** Eight independent source-free ABI mutation checks
were exercised locally using a separately authored synthetic test under
normal Python and `-O` (8/8 each). The separate source-free actor
reference model also passed 20/20 locally. The **committed CTest scripts
and native C++ files have not executed**: source-organization GitHub jobs
continue to fail before a runner executes steps. Local reference checks
are not equivalent to exact Git-blob native ABI/compiler evidence.

Do not mirror this private source into an alternate GitHub organization
just to obtain runner minutes. Platform-conformance #22/#77 and #69
require independent exact-destination/source export approval; no such
approval has been verified here. Use a vetted source-org runner or
approved in-boundary source-local ephemeral machine. Source-free tests
may run separately on a funded org without any private source transfer.

## Native scheduler upgrade admission integration — October 18, 2026

The source branch now adds a **host-only** per-actor DATA admission freeze to
`NativeActorScheduler.c` and a typed `NativeOresVM` supervisor adapter:

- `ores_native_vm_upgrade_freeze(vm, domain, actor, expected_epoch, &ticket)`
  seals admission under exactly the scheduler lock used by `try_send`.
  The historical generation-checked `ActorRef` is not renumbered.
- Newly attempted DATA returns `ORES_NATIVE_SEND_UPGRADE_FROZEN` rather
  than a false ACCEPTED receipt. The legacy index-based `submit` also
  checks the scheduler's frozen flag and cannot bypass the freeze.
- Already acknowledged queued DATA **drains in FIFO order on the old
  handler**, regardless of the freeze; priority CONTROL/terminal STOP still
  progress. A second actor on a different carrier can continue.
- `upgrade_quiesced` requires no active, queued or pending CONTROL work
  and the exact live freeze ticket. `upgrade_finish(..., committed=0)`
  reopens the original route. Only a quiesced `committed=1` advances its
  monotonic route epoch and reopens DATA. Terminal stop/retire permanently
  defeats stale abort/publish tickets.
- Calls from any active native VM actor carrier are rejected, including
  callbacks in *another* scheduler domain. This is carrier-origin hardening,
  **not** an unforgeable supervisor capability or hostile native sandbox.

The new `native_vm_hot_upgrade_mailbox_linearization` CTest exercises
old FIFO drain, rejection after freeze, dedicated CONTROL delivery, second
lane progress, mismatched actor/tickets, code-only simulated version switch,
rollback, stale epoch, stop precedence and actor-origin negative cases.
The existing `tests/source_actor_pic_hot_reload.py` now additionally
compiles/links `NativeActorScheduler.c` so the **actual compiler-emitted
Oreslang actor v1/v2 handlers** are invoked by the real scheduler worker
through `NativeActorHotUpgradeGate`. The v1=42, v2=86 then 128 check
uses the same persistent `OresActorI64Heap` and route epoch, with
matched scheduler/code-gate freeze and drain and rollback of rejected ABI.

**Release blockers remain:**

1. This patch is **not yet executed** on source-owned GitHub Actions.
   A CI workflow with no runner steps is neither a pass nor a compiler
   regression. Run real CTest, compiler-generated PIC, and ASan/UBSan
   before making a source-level capability claim.
2. Supervisor authority is host convention in this current C API;
   a real security boundary must seal CONTROL ownership against
   arbitrary foreign code and prevent untrusted native FFI access.
3. Scheduler physical idleness alone cannot prove guest `await`,
   suspended continuation, external FFI borrow, timer, DMA or shared
   arena quiescence. Compiler lowering must attach **generation pins**
   before such work is allowed to hot-reload (LLVM #606).
4. The scheduler's quiesced observation and the independent code-gate
   publication must become **one linearizable supervisor transaction**.
   CONTROL callbacks can be admitted after an observation; do not
   assume an earlier `quiesced` means future CONTROL has stopped.
5. The test `dlopen` loads only private, trusted, source-generated
   fixtures and is **not** an authenticated production AOT/ORC loader.
   Artifact authentication before constructors and durable unload
   quarantine remain tracked by LLVM #607/#610.
6. Cross-backend Java actor hot swapping and full PRIVATE/UNTRUSTED
   physically protected worker migration remain separate gates.

No private source has been copied into an alternate GitHub organization.
Source export remains on hold under platform-conformance #22/#77/#69.

## 2026-10-18 second audit: admission race and torn-publication failure

Added two CTest-native regressions to draft #627:
- `native_actor_upgrade_admission_contention`: 128 pthread
  producer-versus-upgrade-freeze races on a real `NativeActorScheduler`.
  Every admission must report exactly ACCEPTED or UPGRADE_FROZEN;
  accepted DATA drains once before publish, rejected data is never
  delivered; the legacy index-based entrypoint must not bypass the seal.
  A successful route switch advances the epoch once, and a subsequent
  stop permanently blocks stale ticket resurrection.
- `native_actor_upgrade_partial_publish_terminal_stop`: host code
  generation changes to v2 while scheduler ingress remains frozen. A
  deliberately intervening terminal STOP must cause BOTH route publish
  and attempted old-route rollback to fail. The gate closes and code
  images retire without accepting or duplicating additional DATA.
  This test does not imply recovery from every possible publication
  failure, nor that stop can be rolled back.

**Architectural audit finding:** code-gate CAS and scheduler route CAS
are still separate authority objects. A normal success path has two
publication boundaries. Until a trusted transaction coordinator seals
both with actual compiler-rooted continuation/FFI pins, a failure
*after* code CAS may NOT be treated as a regular rollback. Supervisor
must terminally quarantine/stop or reconcile a versioned transaction
without reopening the old route. CONTROL may still arrive after a
quiescence observation, so a code CAS alone cannot certify a stable
route image. This is a hard blocker, not production implementation.

Evidence: the new tests are committed and registered but remain
**NOT EXECUTED** on the source branch, because the Actions scheduler
records zero job steps. A source-owned rerun of the previous test
run was accepted by GitHub but also recorded zero steps. Local independent
source-free model: 20/20 PASS in normal Python and with `-O`; this is
explicitly not native compiler or CTest execution. A separate funded
organization has real green unrelated workflows, but source export
requires independently reviewed exact source/destination authorization
while platform-conformance incidents #22/#77 and bootstrap #69 remain open.
