# LLVM ActorStream incremental integration (2026-10-12)

This branch is based on exact LLVM current-main commit
`40b4430c0bba01d4a1980270cb90457d910e4835`.
Previous verified nested three-actor guest execution is present in the
merge tree of PR #230 at `995a7f7bc6d33de7c7b2a74aff025a5cbe94beb0`,
but was not incorporated into that current-main compiler/parser/runtime
tree. See issue #253. **Do not overwrite current main's native owned-class,
CFG, ownership or CMake work with the old implementation.**

## What this branch implements

- `NativeActorStreamRoute.hpp`: bounded **2..16** stage scheduler-side
  phase/admission and physical-DONE state machine. Reject duplicate identities,
  wrong nominal domain, foreign senders, replay, out-of-order forward/backward
  calls, forged/stale publication claims, and premature physical actor exits.
- Explicit reservation/commit for DATA publication. A prepublication root
  rejection releases the initial credit; a directional publication failure is
  irreversible. Production scheduler integration MUST commit *at* the DATA
  mailbox publication linearization point, before recipient execution.
- Each stage's routing fence is retained through that specific stage's
  physical retirement; source-only DONE cannot release live sibling fences.
- Host-only C++ regression covers 2, 3, 4, 15, and 16 stages, concurrent root
  claim races, failure, rollback, premature DONE and terminal success.
- Preserved original 3-actor source fixtures from #230 as unchanged Git blobs.
  Preserved `examples/actor-pipeline-15.ores` from Java/GraalVM PR #757
  byte-for-byte to anchor future **identical-source** LLVM parity.

## What is NOT yet implemented or certified

This is **not** a compiled LLVM guest pipeline. `NativeActorStreamRoute`
does not execute guest code, own actors, dispatch real MessagePack frames, or
authenticate scheduler sender identity. It accepts trusted host-supplied
identities and domains. The current main frontend does not parse `define enum`
/ `define pattern` and typed `receive(Message)` like the old #230 branch;
the current bootstrap has only scalar send support.

No certification may treat this C++ state-machine test as compiled guest
execution, signed runtime tracing, or cross-backend parity evidence.

## Next integration gates, in dependency order

1. Port the old tagged/nested enum and `do match` subset into current
   `Parser.hpp`/`Parser.cpp`, preserving the newer owned-class and CFG AST.
   Reuse the compiler-derived exact closed nominal MessagePack v2 shapes.
2. Reconcile `LlvmCodegen.cpp` to produce real source `self.forward()` and
   `self.backward()` under actor lane authority. Wire admission token commit
   to the real scheduler DATA publication boundary (not a host callback).
3. Reintroduce the source actor variant bootstrap capability via additive
   code and CMake integration. Reuse existing native scheduler isolation and
   physical DONE barriers; never copy old bootstrap over current main.
4. Compile/execute `source-actor-pipeline-bidirectional.ores` with three real
   actors, 2 forward/2 backward hops and result 19. Then 2/4/15/16 stages
   with all three source constructor syntaxes. Use exact source Git blobs.
5. Run CTest, ASan/UBSan and AOT/native smoke on exact trees. Cross-compare
   Java and LLVM execution of the byte-identical 15-actor fixture; only then
   enable authenticated FD3 trace conformance in platform-conformance #13.

Release status: integration bridge / native host contract only; keep draft.

## Follow-up authority and lifecycle audit (2026-10-12)

Current scope and constraints:

- Claims now carry an additional monotonic **route generation**. A stale
  publication claim from a destroyed route must fail even if the replacement
  is constructed at the **same native address** with the same first ticket.
  The generation is process-local accidental-reuse fencing, NOT a
  cryptographic credential; the caller remains trusted scheduler code.
- Never silently interpret an invalid `Direction` numeric representation as
  `Backward`. An unknown stage queried for a routing fence is an error,
  not an indication that sending is authorized.
- If the publication ticket allocator ever exhausts its 64-bit space, the
  route becomes permanently failed. It must not retain an unsettled claim
  or wrap back to a claim token that was already issued.
- The route must not report success until its entire forward/reverse phase
  and all physical DONE receipts are complete. Fatal failure wins over a
  pending publication reservation. A sibling's pending valid publication
  must NOT, by itself, cause another finished stage's physical DONE to fail.
- **Integration obligation:** bind each actor generation to exactly one
  live pipeline in the owning runtime's actor registry, and authenticate the
  exact causal sender from the admitted DATA turn. These are NOT supplied
  by the host-only state machine. Guest data must never supply actor IDs,
  route generation, sender identity, stage indexes, or publication claims.
- **Publication obligation:** a claim must be settled at exactly the real
  mailbox DATA publication point. A caller that cannot confirm whether
  publication happened must permanently fail/quarantine the route; it must
  never assume pre-publication rollback when delivery may have occurred.
- **Shutdown obligation:** every reserved publication must be committed,
  rejected before publication, or terminally aborted; physical retirement
  receipts must be taken from scheduler-owned generation-aware finalizers.

### Verification evidence

- [Funded native exact-source run #38319006169](https://github.com/zed-pkg-test/java-app/actions/runs/38319006169):
  GCC and Clang ASan/UBSan passed; 50 executions per configuration.
  Tested allocator-address reuse, stale root claims, and post-failure
  settlement rejection.
- [Funded native exact-source run #38319194539](https://github.com/zed-pkg-test/java-app/actions/runs/38319194539):
  GCC and Clang ASan/UBSan passed; 50 executions per configuration.
  Adds rejection of forged direction enum values and unknown-stage
  admission-fence queries.

These are exact-source *host route* tests. Full CMake/CTest, scheduler
integration, source-compiled LLVM actor execution, native trace provenance,
and Java/LLVM source-identity parity remain unverified on this PR.

## Terminal physical-DONE race audit (2026-10-12)

A complete pipeline once returned `Done::Succeeded` after every physical
DONE receipt, but a subsequently delivered `fail()` event could change its
observable result to `Done::Failed`. That violates terminal-result
monotonicity for an already completed, fully quiescent pipeline.

The route now serializes the final scheduler-owned physical DONE receipt and
terminal failure publication under the same mutex. A failure **before**
successful whole-route physical completion remains fatal; a late failure
**after** successful completion cannot revoke the observed success. Repeated
late failures are idempotent. This is an ordering contract for authenticated
scheduler notifications, not permission for a guest to suppress a real
failure or fabricate a physical-DONE acknowledgment.

The C++ regression now checks each stage's physical-DONE threshold at **every**
phase 0..2N of the 2-, 3-, 4-, 15- and 16-stage pipeline routes. Premature
physical termination must poison the route; eligible termination must not
unregister surviving stage fences. It separately tests 100 concurrent final
physical-DONE versus failure publication races per binary execution and checks
that the terminal decision cannot change afterward.

**Funded exact-source execution:** [CI #38320002813](https://github.com/zed-pkg-test/java-app/actions/runs/38320002813)
verified the header Git blob `734e889647cc0c951297bc1f718c5a345e50849d`
and test Git blob `e815aa02476a6cab9b32098ba02c22bf3c80a835`;
GCC and Clang ASan/UBSan each passed 50 binary executions.

**Unchanged release blockers:** the tests cover only the standalone native
route contract. Source-compiled LLVM guest pipelines, scheduler-integrated
exact publication receipts, all-stage native physical STOP/DONE evidence,
full current-head CTest/ASan, and same-source Java/LLVM trace conformance
remain unverified. Leave PR #262 in draft until those gates are closed.

## Typed final-result receipt — native host contract (2026-10-12)

The bounded `NativeActorStreamRoute` adds `complete_source_with_reply`
and `completed_reply`, without claiming that the compiler emits either API.

- Only a trusted scheduler possessing the **actual source identity, immediate
  downstream sender**, and exact closed nominal mailbox domain can commit the
  final reply. The runtime must first validate the real canonical MessagePack
  v2 payload against the compiler-owned schema and provide its owned frame.
  These fields are not supplied from guest message bytes.
- Result commitment and source terminal phase advance are serialized by the
  same mutex. The frame is copied before the route reaches terminal phase;
  failed allocation cannot publish a missing final result. Empty and
  over-1MiB result frames are rejected. This is an additional bound, not
  permission to bypass the mailbox's tighter schema quotas.
- `completed_reply()` releases only a fresh copy and only **after the
  full forward/backward route and every physical stage DONE have succeeded**.
  A pending/failed route, forged sender, late duplicate, or old legacy
  payloadless completion cannot masquerade as a typed result.
- The legacy `complete_source` remains for existing control-plane tests.
  No inferred final payload is produced on that path.
- Adversarial native tests cover 2/3/4/15/16 actors, wrong domain/sender,
  empty reply, duplicate commitment, mutable caller buffer, returned buffer
  mutation, premature result reads, route failure after reply, and late
  failure after fully committed success.

**Integration gap:** This C++ contract is a host-only state machine. LLVM
must still lower actual guest return envelopes, bind them to scheduler-owned
actor IDs and canonical MessagePack codecs, and execute source-compiled
`PipelinePlanner`/`PipelineController`/`PipelineRunner`. A host test does
not demonstrate equivalent source-compiled LLVM guest behavior.
