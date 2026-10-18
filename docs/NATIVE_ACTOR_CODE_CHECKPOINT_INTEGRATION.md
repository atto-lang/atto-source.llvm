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
