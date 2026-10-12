# LLVM native actor heap retirement: audit contract

The bounded `ActorHeap` in stacked draft #51/#228 is a **native byte
allocator prototype**, not compiler-lowered arbitrary `.ores` actor state.

## Required invariants

- Every heap gets a nonzero, nonreused identity. Handles bind to that heap,
  slot and generation. A handle from another heap, old generation or wrong
  carrier cannot access or free native storage.
- Reads/writes stay within the occupied slot's declared length. A reclaimed
  slot is scrubbed and zeroed before reuse. Slot retirement increments or
  permanently exhausts its generation, never wraps silently.
- Exactly one admitted carrier owns this prototype's mutable heap.
  **Logical ActorId-based carrier migration is not yet supported by this
  standalone HungryCarrier-bound allocator.** Do not silently treat it as
  suitable for migrating general actors.
- The supervisor must establish quiescence: fence new messages and await,
  cancel outstanding continuations and FFI callbacks, return borrows, and
  ensure no native users remain before destroying the arena. Calling
  `ActorHeap::retire()` is not itself proof of quiescence.
- Retirement runs only on the bound carrier. It is monotonic/idempotent,
  scrubs every slot, revokes existing handles, forbids new allocations, clears
  accounting, and cannot mutate a neighboring actor's heap.
- There is no tracing GC on this allocator path; this does **not** establish
  zero allocations, hard realtime, OS security isolation, or source-level
  language memory safety.

## Executed checks

- GCC C++17 with ASan+UBSan and Clang C++17 with UBSan, using an exact Git-blob
  mirror in the funded test organization.
- 12,000 deterministic allocator-model transitions with independent expected
  counts, stale-handle rejection, scrubbed reuse, mixed writes and retirement.
- Multi-thread carrier admission and foreign heap denial.

## Still blocking

Full LLVM CMake/CTest and source actor lowering, formal quiescence/lease and
continuation cancellation coordination, deterministic destructor lowering,
cross-backend alias/sendability conformance, and stronger process/isolate
security tests. This PR must remain draft until those integrations execute.
