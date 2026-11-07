# LLVM actor-owned scalar guest frame M2 — stacked compiler slice

Status: DRAFT, compiler/runtime integration written, **NOT_EXECUTED** at exact head until a source-authorized runner completes actual compiler and linked guest tests.

Dependency: LLVM actor-owned logical frame store PR #1536. This branch MUST be reviewed/merged after its parent; do not squash or promote to main independently.

## What M2 implements

The real `oreslang-llvmc` compiler has an opt-in `--experimental-actor-guest-frames` mode for **one standalone `.atto` actor declaration** with:
- exactly one `on_message(int): int` value parameter and a synchronous scalar expression;
- 1..16 native scalar actor heap fields;
- no free functions or source `main`, classes, imports, sum types, suspend/await/select, owned temporaries, early-return control flow, or declared effects;
- host native only; no Wasm, Cortex-M, hard-real-time certification, or tree shaking.

For that narrow source subset the LLVM emitter calls `oreslang_actor_guest_frame_i64_enter(message)`, `...read()`, then `...leave()` around the generated actor handler. These are compiler-only C ABI imports implemented in the **real** `MultiActorCarrier.cpp` runtime. The runtime never trusts a guest-supplied pointer, actor id, epoch or stack token: its existing exclusive worker callback creates the authenticated TLS scope, validates the actor incarnation, and reads/writes its own bounded `ActorGuestFrameStack`. The guest receives no pointer to backing memory. Invalid access terminates rather than returning borrowed memory.

The i64 parameter is **materialized and read from actor-owned logical frame bytes**. The immediate synchronous evaluator still copies the checked value to a transient carrier-stack slot for expression lowering; so M2 does **not** eliminate every use of the physical C call stack, lower a full stackless function call graph, or support persistent local frames across source-level suspension. Permanent actor fields remain in distinct zero-GC `ActorOwnedI64Heap` instances.

## Test contract

`tests/fixtures/actor-guest-scalar-frame-m2.atto` is real guest source, compiled into `oreslang_actor_Counter_init` and `oreslang_actor_Counter_on_message`. A trusted C++ test adapter sets up **two different actor incarnations with two distinct physical heap allocations and frame stores**, registers both on the **same real native carrier**, and calls the actual generated guest handler from the carrier callback.

The test admits A:7 and B:100, waits for real compiled results, cancels and **physically reclaims A's heap**, then executes B:2 and B:-4 while B remains live. It checks expected A=7, B=100->102->98, own-token authorization, sibling-token rejection, no retained frames after completed handlers, exclusive carrier execution, per-actor completion counts and final physical cleanup.

`tests/native_actor_guest_scalar_frame_m2.sh` must:
1. reject mixed actor + source main without emitting **any LLVM IR**;
2. require compiler-defined actor entrypoints and all three frame ABI calls in emitted IR;
3. deny unexpected `llvm.gcroot`, `llvm.experimental.gc.statepoint` or a GC strategy marker in the **emitted module only**;
4. actually run linked native Clang -O0 and -O2 guests; optional Clang ASan/UBSan covers host and native components and checks the same receipts;
5. emit exact fixture/compiler/binary SHA-256s.

`.github/workflows/actor-guest-scalar-frame-m2-exact-head.yml` checks immutable source blobs, compiles the real LLVM frontend on the source repository's authorized runner, demands three actual guest process passes, checks exact-head/tree and uploads same-run receipt. **Zero executed steps = NOT_EXECUTED**, never a success.

## Not certified

This milestone does **not** execute source `spawn`/`ActorRef`/source main; it uses a trusted native actor adapter around a genuine compiled AttoLang handler. It does not prove `await`/`select` continuation spill/reload, borrowed-lifetime fences, actor-call recursion, dynamic typed frame layouts, async cleanup, full linked-image GC absence, OS-level physical stack isolation, sandboxing, hard real-time WCET, or Java backend parity. It must remain draft until genuine guest execution is observed.

## Next independent gates

- Merge parent actor frame-store substrate only after stepful tests and security review.
- Execute native compiler and compiled guest CTest O0/O2 and ASan/UBSan at this PR's exact SHA/tree, with independent ORESoftware review.
- Integrate actor-frame ownership into the **source ActorRef/SourceActorBootstrap** scheduler path rather than only the MultiActorCarrier host bridge, preserving actor identity/heap epoch proof and STOP retirement.
- Lower full scalar locals/call frames, then resumable `await`/`select` with typed continuation ownership; add real native AOT smoke on both backends.
- Compare precise results to Java PRs #2624, #2628, #2633, #2639, not just contract metadata.

No source export, cross-organization CI mirroring, token transfer, protected-main write, or release-certification claim is authorized by this PR.
