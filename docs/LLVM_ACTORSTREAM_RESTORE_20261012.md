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
