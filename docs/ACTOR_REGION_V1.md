# ActorRegionV1 — bounded native actor-region bridge (host substrate)

**Status:** host-side implementation only; **not** a compiler-backed `.atto`
actor-object allocation, deterministic drop-lowering, a fully closed AOT
runtime link manifest, or complete process-wide zero-GC certificate.

`ActorRegionV1` wraps the existing, independently maintained `ActorArena`
implementation; it **does not replace or delete** the established i64 heap,
byte heap, actor arenas, scheduler, or their APIs. The arena reserves its backing
slab, metadata and scratch at startup, enforces one authenticated carrier turn
at a time, and zeroizes dead slot bytes. Each region has a fixed physical slot
stride and a bounded slot count: current hard limits are 4,096 slots, 1 MiB per
slot, and 64 MiB per actor region including scratch. No tracing collector or
per-allocation malloc is needed during an actor turn in the backing arena.

## Encoded 32-bit guest offset and trusted proof

`OresActorOffset32` has exactly **four bytes**. Its nonzero value is `1 +
slot_index × slot_stride`, so zero remains null and a byte location can be
represented without exposing a native host pointer. This is *not* an
independently authorized pointer. `OresActorRegionV1Ref` carries the original
arena `domain/index/generation` as a **trusted out-of-line proof**. A valid
actor turn and that exact proof are required for read, write and drop; offsets
are checked independently against the original proof. A guest must never be
allowed to manufacture a proof from an offset, or copy these host tokens via
channels, FFI, or pub-sub. The source compiler must lower ownership and member
access to this or a semantically equivalent checked ABI before guest exposure.

## Lifecycle

At initial allocation, the backing arena memory is zeroed. Every drop scrubs
the entire physical slot, advances the generation, and permits reuse without
resurrecting the previous reference. A shared read loan may survive an actor
turn and prevents writes/drops. Retirement fences new accesses and destruction
waits for active turns and loans. The supervisor must still fence timers,
continuations, actor handlers and FFI before the final destroy: this C ABI
**does not** prove those external obligations. Termination uses fatal abort on
unauthorized access, and must not be exposed directly to untrusted callers;
production guest diagnostics, cancellation and supervisor recovery must add a
controlled fault boundary.

## Validation and AOT integration still required

- Run `native_actor_region_v1_offsets_generation_drop` with real native
  `ActorArena.c` and `ActorRegionV1.c` on an exact source head; run all abort
  cases in isolated child processes. Existing arena tests must still pass.
- Add compiler lowering of real `.atto` object allocations and field access
  with explicit borrowed/owned lifetimes and cleanup on every CFG edge.
- Produce source-authenticated actor turns and prove no API leaks forgeable
  native authorization metadata into guest memory.
- Admit this native runtime to sealed AOT **only after** a transitive symbol,
  ownership, cleanup and FFI audit. Preserve the current fail-closed sealed
  linker restriction until then.
- Exercise task/actor cancellation, exception and supervisor restart,
  repeated reuse and hot-upgrade quiescence under a real scheduler.
- Prove cross-target parity in GraalVM, .NET NativeAOT, Wasm and Cranelift;
  this host-only test is not evidence of guest parity or zero GC in those VMs.
