# Wasm32 actor-owned scalar guest frame — M2 narrow compiler milestone

Status: **DRAFT / execution NOT_EXECUTED on exact head**. Parent LLVM PR #1350 (checked native recursive scalar call containment). Companion Wasm pooled frame bridge PR #416 and private arena security PR #418 are separate, unmerged work.

## What the LLVM compiler now emits

For the existing admitted **one-actor / one-instance, synchronous, import-free** `--target=wasm32` actor subset, `LlvmCodegen.cpp` emits one bounded, internal actor scalar frame global:

```llvm
@atto.wasm.actor.scalar.frame.v1 = internal global [N x i64] zeroinitializer
```

`N = 1 + actor.fields.size()`, so there are 2..17 cells (16 actor fields maximum) and at most 136 bytes of logical frame payload. The compiler stores the incoming i64 message and each actor field snapshot into this instance's *private Wasm linear memory* using LLVM volatile stores. The actor handler reads/mutates the slot pointers through ordinary expression lowering, commits actor fields to persistent instance state, then emits volatile zero stores for **each** frame cell before returning its i64 result. It does not emit `alloca` for the actor's message and field snapshot slots. The normal guest lifecycle remains `init -> on_message* -> teardown`; all exports retain their existing signatures and no new imports/exports are added.

This is a **compiler-generated guest-owned logical scalar frame**, not the native WebAssembly engine call stack. LLVM and Wasm engines still have their own transient control/operand stacks; the emitted guest frame is a distinct bounded region *within the actor's own private exported linear memory*, not a separately allocated WebAssembly.Memory. A physical worker may host multiple independent instances; one actor cannot access another instance's frame through this ABI.

### Scope limits

- Each guest instance admits only one synchronous active handler at once. The fixed source actor ABI still does not support recursion within the actor handler, suspension, await/select, dynamic subframes, borrowed objects, arbitrary class layouts or recovery after a trap. There is no safe stack migration or supervisor CONTROL protocol.
- A trap during guest execution is fatal to the current trusted pooled Worker; no guarantee of scrubbing a still-running or trapped engine call stack. Physical Worker termination and quiescence before release are required by the pooled host gate.
- The separate host `ActorFrameLedger` in Wasm PR #416 records host physical turns only and is **not** automatically mapped to compiler-generated guest slots. They must be reconciled under a future, versioned ABI before guest continuation migration.
- Preserve private actor state across turns. It is **incorrect** to require all Wasm linear memory to become zero after each message; only the compiler-owned frame slots are zeroed. Actor field state persists until teardown.
- Reserved memory: the already-approved private, non-shared two-page (128KiB) guest memory profile. These 2..17 static i64 cells consume part of that memory. Linker still has final authority over private layout and exact memory size.

## Required executed evidence

The new canonical `tests/fixtures/wasm32-actor-owned-scalar-frame.atto` source, Node/Clang test `tests/wasm32_actor_guest_frames.mjs` and CTest `wasm32_atto_actor_owned_scalar_frame_guest` require **actual**:

1. Exact current `oreslang-llvmc --target=wasm32` compilation of the `.atto` fixture, with no implicit native imports.
2. LLVM IR inspection for internal bounded private frame allocation, compiler-generated GEP slots, volatile initialization and final scrubbing; reject silent fallback to an alloca-backed message slot.
3. Clang/LLD wasm32 link under both `-O0` and `-Oz`, exact four exports, zero imports, fixed non-growable two-page memory.
4. Node WebAssembly execution with two independent instances, lifecycle denial before INIT/after STOP, state independence and success with signed 64-bit messages. Print fixture/IR/Wasm SHA-256 digests on success.
5. Independent verification that source trees, compiler binary and guest Wasm correspond to this exact PR head. A source-org workflow failing before steps counts as **NOT_EXECUTED**.

Example on a source-approved runner with compiler and Clang available:

```sh
node tests/wasm32_actor_guest_frames.mjs \
  --compiler /verified/source/build/oreslang-llvmc \
  --clang /verified/clang \
  --fixture tests/fixtures/wasm32-actor-owned-scalar-frame.atto
ctest --test-dir build -R '^wasm32_atto_actor_owned_scalar_frame_guest$' --output-on-failure
```

No private source export to an unauthorized CI carrier; no merged branch and no release parity until real guest runs, independent review, and downstream ABI receipts.

## Following M3 gates

- Introduce typed, versioned guest frame metadata, bounded dynamic function frames, and compiler control-flow proofs for returns/raise/traps; reject unsupported recursion before emission.
- Integrate opaque actor/task incarnation + continuation generation and scheduler-owned await/select/cancel with Wasm #416. Host and guest memory cannot be exposed as raw cross-actor pointers.
- Prove actual `.atto` actor A recursive stack-budget failure while actor B remains responsive on a pooled carrier, plus supervisor CONTROL, restart with clean state, stale-ticket denial.
- Match exact canonical source fixture semantics with Java/GraalVM and LLVM-native; obtain Wasmtime AOT execution and private CI authorization.
