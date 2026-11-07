# Wasm actor checked recursive frame trampoline v1 (experimental)

**Status:** Authored compiler implementation, **NOT_EXECUTED** as an exact-head source-compiled guest until a runner executes actual steps. Opt-in on top of LLVM actor-owned scalar guest frame M1 PR #1546. This is not generalized recursion or full Java/LLVM/Wasm actor-stack parity.

## Trusted source/compiler boundary

`oreslang-llvmc --target=wasm32 --experimental-wasm-actor-recursion-v1 tests/fixtures/wasm32-actor-bounded-recursion-v1.atto`

The compiler accepts exactly one actor with one zero-initialized scalar field, a synchronous `on_message(int)` returning the result of a **single** pure same-module helper `fnc descend(int)`. Helper shape must be precisely:

```atto
fnc descend(int depth): int {
  return depth == 0 ? 0 : 1 + descend(depth - 1);
}
```

An unobservable constant main returning 42 is required by the source profile. The emitter checks actual parsed, validated AST structure, not comments, filenames or string search; all different recursion, extra side effects, additional functions, actor writes, owned/borrowed locals, asynchronous calls, imports, alternative arguments and non-wasm32 targets reject *before any executable IR*. No generic scalar compiler path silently acquires the option.

## Actor-private guest machine

The special LLVM emitter lowers that specific recurrence into **one bounded iterative source function** with explicit blocks for push, descend, unwind/pop and quota failure. It never emits a recursive call to the Wasm engine; generated IR must not declare/define an executable `descend` function.

- Each distinct `WebAssembly.Instance` has a private `[64 x i64]` frame-slot array in fixed two-page linear memory, plus private depth, terminal status and lifecycle state.
- `on_message(63)` pushes exactly 64 source frames, unwinds each, zeroes every frame slot, returns signed-i64 `63` and leaves status 0.
- `on_message(64)` attempts a 65th push. The guard denies it before any out-of-bounds GEP, zeroes the entire frame array, resets depth, sets lifecycle TERMINAL and status=1, then raises a Wasm trap. It never returns an ordinary i64 `75` or other sentinel.
- A new **read-only, zero-argument i32** guest export `oreslang_wasm_actor_frame_status()` reports status 0 (normal) or 1 (typed stack budget exceeded). This is the **opt-in five-export ABI**: init, on_message, teardown, frame_status and private memory. Legacy scalar actor retains four exports.
- The Wasm host must validate exact export signatures and no guest imports. After an actual trapped turn, only an exact terminal status=1 and a quiescent actor-owned host frame can authorize actor-local cleanup and `STACK_BUDGET_EXHAUSTED_V1` receipt. Any invalid status, unknown trap or cleanup failure stays carrier-fatal. The host may not infer terminal failure from a guest return value.

This is a bounded source-recursion trampoline for the *specific certified recurrence*. It has a small nonrecursive C++-emitted Wasm activation and does not invoke a nested OS/Wasm engine call for each source frame.

## Compiler-backed acceptance and negative tests

`tests/wasm32_actor_checked_recursive_frames.py`, registered as CTest `wasm32_compiler_actor_checked_trampoline_v1`, requires the native compiler actually built from exact source and Clang wasm32 and Node 22. It compiles canonical `.atto` source Git blob `947781cddf09d8e3e08c51cff2b7d8960e2d4750` into LLVM IR, verifies the push/unwind/overflow machine and absence of native recursive calls, then links/runs real Wasm at Clang **O0 and Oz**.

The Node guest test runs two separate instances, 64 admitted frames, 65th denial with i32 status1 and terminal lifecycle, independent instance B continuing, fresh C with zero state, and exact no-import five-export ABI. It reports source/compiler/IR/Wasm digests and explicitly marks **same-worker pooled fault isolation and supervisor CONTROL NOT_TESTED by the direct-instance compiler test**.

Negative compiler tests reject missing/duplicate flag, non-wasm32/alternative tree-shake modes, wrong recursive decrement, changed unwind expression, extra actor state write, a constant rather than actor argument, and multiple recursive call sites. Rejected source must not produce LLVM IR. Workflow `.github/workflows/wasm-actor-checked-recursion-v1.yml` builds at exact PR head and requires two independent compiled guest receipts; zero-step workflow is `NOT_EXECUTED`, never a failed language test.

## Still missing

- This milestone does **not** implement arbitrary direct/mutual/indirect recursion, proper tail-call optimization, full source-frame PCs and operands for arbitrary expressions, ownership roots, suspension, actor-local async continuations or method cycles.
- The Wasm pooled worker's same-carrier actor A/B cleanup test belongs in a **separate Wasm integration PR** and requires the authenticated native compiler produced by this exact LLVM head. Source/binary digest relation must be independently attested, not guessed from CLI flags or source metadata.
- True supervisor CONTROL delivery and automatic restart are not implemented by this source-contained slice. Spawning an actor C with fresh memory does not by itself prove supervisor restart.
- Node Workers do not establish an OS sandbox, no-GC whole-system memory policy, or protection against arbitrary untrusted native Wasm code.

**Merge hold:** retain draft and independent ORESoftware review, source-org exact-head nonzero-step compiler/Node/Clang execution, and semantic reconciliation with LLVM #1350/#1537/#1541/#1542 and Wasm #412/#413/#415/#417/#420. No cross-org private source mirroring or billing/review bypass.
