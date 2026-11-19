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

## Exact-blob local native compilation evidence (host-only)

Authoring validation on Linux x86_64 (kernel 6.18.57), with
Clang 17.0.0 and GCC 14.2.0, compiled the *same bytes* that were
committed to this source branch:

- Existing nominal contract Git blob:
  `d37b500299e402684c6fb66bd890c55472faa5bf`.
- Nested plan header Git blob:
  `6990d9cda166e05e615faf1a532d2e1f5712b611`.
- Native test Git blob:
  `fec517db761d28dadac9c0557298b1b19054bd91`.

These source hashes were independently computed using `git hash-object`
on the local C++ files and matched the uploaded GitHub blobs. The standalone
C++17 test compiled using `-Wall -Wextra -Wpedantic -Werror -UNDEBUG`
and **executed successfully**, without skipped test steps, under:

| Compiler | Optimization/instrumentation | Executed outcome |
| --- | --- | --- |
| Clang 17.0.0 | `-O0` | PASS |
| Clang 17.0.0 | `-O2` | PASS |
| GCC 14.2.0 | `-O0` | PASS |
| GCC 14.2.0 | `-O2` | PASS |
| Clang 17.0.0 | `-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer` | PASS |

The sanitizer run used `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1`
and `UBSAN_OPTIONS=halt_on_error=1`. Recorded SHA-256 hashes:
Clang O2 binary `bb847aa5a09147e51472ddf2fbef76396f887be8ed58cb279733669226ff5693`,
GCC O2 binary `865a13a43d5d99de6ccf03ecfd96caed57cd97e5c8275961f092f2040b1c3948`,
sanitizer binary `3d91bbb00515e42237764cd5d3b38e0a3803e5f1516650a432b052ae9508eeae`.

**This is a local standalone native-header regression, not a complete CMake
build, LLVM IR generation, actor-runtime execution, full guest AOT,
GitHub Actions execution, or five-backend parity.** The current environment
does not have an LLVMConfig.cmake needed to build the full LLVM compiler.

At initial PR head `0d4d33862d65bbf08fd073f77e2c43bad20e77b2`,
the source-org LLVM compiler
[run 41754205684](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/actions/runs/41754205684)
and conformance
[run 41754205765](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/actions/runs/41754205765)
completed unsuccessfully with **zero job steps**. They provide no
source-test verdict.

## Java fail-closed admission alignment

The Java/Graal backend merged
[PR #2512](https://github.com/ores-truffle-oreslang/oreslang-source.java/pull/2512),
which explicitly rejects `Actor.pipe` and `Actor.connect` as compiler-reserved
intrinsics rather than accidentally falling through to dynamic method invocation.
This LLVM draft adds the corresponding lexical-token-aware primary-expression
gate for `Actor.pipe(...)`, `Actor.pipe<...>(...)`, `Actor::pipe(...)`,
`Actor.connect(...)`, and `Actor.connect<...>(...)`, plus six negative
`.atto` source contracts counting the recursive case.

These gates establish a **proposed common fail-closed boundary**, not positive
execution parity. The LLVM tests require a built native compiler and are still
unexecuted because source-org Actions jobs show zero recorded steps. Ordinary
`Other.pipe(...)` expressions are not caught by the reserved Actor namespace
guard; their separate native type admissibility remains governed by the
existing compiler.

## 2026-11-04 hardening audit: immutable metadata and resource bounds

The new source-free planner revision:

- Makes `NestedPipePart` construction state inaccessible to external
  mutation. The only legal constructors are the `actor(...)` and
  `group(...)` factory methods; descriptors remain move-only.
- Makes `NestedPipePlan` non-default-constructible outside the validated
  planner, with **const-only** accessors for actor/edge/supervisor lists
  and total-slot receipts. `nested_pipe_restart_targets` can no longer
  accept an arbitrarily constructed/edited instance from ordinary C++ callers.
  This still **does not** turn metadata into an authenticated runtime
  supervisor ticket; the actual runtime must verify actor incarnations,
  caller authority and current generations independently.
- Binds the *number of nested group segments* independently of actor
  stages (default/hard maximum: 2,048 segments) to prevent excess
  all-group tree expansion. Retains maximum 64 depth, 1,024 actors,
  1,023 DATA edges, 256 analysis scopes, 65,536 messages/edge, and
  1,000,000 aggregate forward DATA message slots.
- Bounds actor type and port labels at **256 bytes each** and rejects
  NUL/control bytes before copying into plans or diagnostics. Full Unicode
  validity/normalization and type identity remain compiler responsibilities,
  not guarantees of this standalone host planner.
- Adds static assertions and C++ adversarial test cases for forged plan
  creation, mutable accessors, over-limit segment admission, oversized
  metadata and embedded NULs.

**Evidence boundary:** New C++ hardening has been committed but still
requires *fresh execution* of exact-head standalone GCC/Clang+ASan/UBSan
tests. Earlier five local native passes apply only to the older
`6990d9c...` planner/`fec517d...` test blobs, not the updated ones.
The `.atto` source fixtures remain expected compiler rejections, never
positive guest factory execution. The full LLVM/CMake + CTest build and
source-org Actions remain unverified unless fresh stepful logs are available.

The branch was semantically reconciled with `main` using main's
parser/CMake files as the base and adding only the feature's
separately delimited admission and CTest gates. This preserves
unrelated recent LLVM work.

## Reproduction of the hardened model

On an approved, source-local environment with Clang and GCC, execute:

```sh
bash scripts/test-nested-actor-pipe-host.sh
```

This fails if either compiler is missing or any of the five host
executions fails (Clang/GCC O0/O2 and Clang ASan+UBSan). The script
prints `SOURCE_FREE_ONLY`; it does not build `oreslang-llvmc`.

The strict CTest suite also registers eight `.atto` compiler-denial
cases for unsupported `Actor.pipe`/`Actor.connect` spellings and
one **positive** comment-only lexical-control `.atto` fixture that
must compile and exit 7 under Clang. These compiler cases have not
yet been executed against the hardened branch; no green checks may be
inferred from merely registering them.

## Bounded plan validation complexity

The validated native plan now retains compiler-produced numeric
`source_index`/`target_index` for each sequential DATA edge, checks
adjacency and source-generated path identity before returning the plan,
and sorts edges by those indices. This replaces repeated actor-path
string scans during link validation and comparison with bounded
O(stages + edges log edges) planning, within the declared budget. These
indices are **not** runtime actor IDs or authority tickets. The
production compiler and scheduler must still authenticate every
generation and connection separately.
