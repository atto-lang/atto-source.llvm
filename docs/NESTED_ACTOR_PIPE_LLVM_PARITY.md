# LLVM nested actor pipe parity: incremental current-main gate

**Status (2026-11-03): NATIVE PLAN ONLY / GUEST PIPELINE NOT VERIFIED.**

This branch adds a source-free C++17 nested graph-plan validator
(`NestedActorPipelinePlan.hpp`) and adversarial native host tests. It
takes structurally prepared nominal `PipeStage` metadata, flattens nested
groups, checks the existing `ActorPipeContract.hpp` role/payload contract,
tracks per-edge capacities and direct-child supervisor registration order,
and rejects invalid resource budgets.

**It does not implement `Actor.pipe(...)` or `.start()` in the AttoLang
compiler**. Current LLVM source explicitly rejects a tokenized `Actor.pipe(`
expression with `NESTED_ACTOR_PIPE_LLVM_NOT_IMPLEMENTED`; the rejection
is independently registered as a negative `.atto` compiler CTest. An
untyped host model must never masquerade as successful guest codegen.

## Source and parity boundary

- Authority: [AttoLang spec draft PR #3](https://github.com/ores-truffle-oreslang/attolang-language-spec.md/pull/3),
  with Chapters 14/15; the deferred builder change is still under review.
- Native toolchain: current LLVM `main`, not a rebased copy of old
  [typed actor pipeline PR #345](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/345).
- An explicit top-level `.with_supervision(#one_for_all)` is a new
  runtime-minted supervisor under authorized caller ownership; without it,
  graph actors inherit the authorized caller's existing supervisor.
- Plain nested groups do **not** mint a supervisor. An explicitly supervised
  nested group is one direct child in `#one_for_all` / `#rest_for_one`.
- Capacity applies to INTERNAL edges at the nearest explicit segment scope;
  cross-boundary edges are owned/capped by their parent segment.
- Model limits: 64 nested groups deep, 1024 actor stages, 1023 forward
  edges, 256 group scopes, 65536 messages per edge, 1000000 aggregate slots,
  with independent runtime **byte/arena/CONTROL** quotas still required.
- No `ActorRef`, port ownership/serialization checks, actor constructor
  capture, aggregate bytes, state/heap authority, reverse `self.backward`
  replies, scheduler admission or actual restart are implemented here.

## Required follow-up implementation order

1. Ratify the deferred builder ABI and distinguish it from immediate infix
   `pipe`, including the effect/Result-or-Future carrier of `.start()`.
2. Add compiler-owned `ActorConstructionDescriptor` and
   `NestedActorPipeExpr` AST nodes with exact effect and move-only
   constructor capture admission. Explicit nested `.start()` is rejected.
3. Lower resolved actor roles and **both forward and correlation-bound
   backward DATA port contracts** to statically typed graph-plan metadata.
   Do not use dynamic class-string reflection or erase backward roles.
4. Compile the plan into immutable AOT constructor/port registries; connect
   per-edge count **and byte** reservations to real scheduler/mailbox
   authority. A plan is not itself permission to spawn.
5. Implement one transactional outer `.start()`: prepare quotas and
   supervisor CONTROL receipts, initialize staged children, publish the
   graph once, and roll back failed/cancelled starts with physical retirement
   or explicit quarantine.
6. Fence graph edges on actor/supervisor restart; reconnect only after
   new generations are fully admitted. Preserve DATA/CONTROL separation and
   `#rest_for_one` direct-child selection.
7. Add genuine positive and negative `.atto` compile-and-execute
   fixtures: 4 stages/3 typed edges, flat equivalence, inner 16 vs
   outer 1024 edge capacity, nested supervisor restart, reverse replies,
   closed/full queues, stale tickets, constructor effects, failure rollback.
   Use Clang O0/O2 native AOT, ASan/UBSan and exact-head CTest on an
   approved source-local runner; never count zero-step jobs as verification.

Do not copy private source into another organization solely to obtain Actions
minutes. Any transfer requires separate approved exact source tree/destination
authorization and security governance.

## Verification classification

- `native_nested_actor_pipe_plan_host_only`: source-free, real C++17 host code
  (not native AttoLang guest execution).
- `native_nested_actor_pipe_guest_failclosed`: expected compiler rejection.
- True `Actor.pipe` LLVM parity: **NOT_IMPLEMENTED / NOT_VERIFIED**.
