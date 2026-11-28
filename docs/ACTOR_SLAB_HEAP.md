# Actor slab heap: a bounded allocator that makes 32-bit refs save memory

`runtime/ActorSlabHeap.hpp` is the first production-shaped native allocator for
actor heaps. It exists because a 4-byte reference only reduces memory if the
allocator's own metadata and fragmentation do not consume the saving. The
earlier prototypes did not pass that test: `ActorOwnedHeap` kept 36 bytes of
slot metadata per object, and `ActorCompactFieldStore` kept a 48-byte proof per
edge.

## Decisions

**Two reference forms, one word size.** Both are 4 bytes on every host.

| Form | Encoding | Use | Per-object cost |
| --- | --- | --- | --- |
| Offset32 | `offset/8 + 1` (`CompressedActorRef`, unchanged) | compiler-proven actor-local lifetimes; the default | none |
| Handle32 | `(generation << slot_bits) \| (slot + 1)` | where stale-reference protection needs indirection | one 8-byte table entry |

* Offset32 is the cross-backend word. Java and .NET must use the same word for
  the same role; .NET PR #437's `gen16 | slot16` scheme is a Handle32 profile.
* Offset32 detects null, foreign, interior, unallocated and freed refs (through
  the page bitmap). It cannot detect ABA, because a freed offset can be
  reallocated. That is why it is reserved for compiler-proven lifetimes.
* Handle32 slot bits are sized to the configured capacity, not fixed at 20/12.
  A 1,024-entry table leaves 21 generation bits (about 2 million generations
  per slot). Free slots are reused **FIFO**, so all slots age at the same
  rate, and the table supports about `capacity × 2^generation_bits`
  allocations before any slot exhausts. An exhausted slot is **quarantined**:
  it is counted in `stats().quarantined_handles` and never wrapped or reused.
  Recovering quarantined slots requires a new heap incarnation (actor
  restart), because stale words may still sit in actor memory.
  `handle_generation_bits` selects a narrower profile deliberately.

**Allocator shape.**
* The region is reserved address space, committed in granules on demand up to
  a budget. There is no 4 GiB cage. `ActorHeapRegionPool` maps once for many
  actors, which avoids per-actor kernel mappings (100k actors would exceed
  Linux's default `vm.max_map_count`).
* 4 KiB slab pages, one size class per page. There are 24 classes from 16 to
  2,048 bytes, with at most 12.5% rounding above 128 bytes. Larger objects use
  page runs. A 64-byte in-page header holds the class, live count, page-local
  free list and allocation bitmap. Page *kinds* live out of band, because a
  large object's tail pages are payload and must never be read as headers.
* Pages that empty out return to a shared free-run pool and serve any class.
  Freed bytes are zeroed, and STOP scrubs carved pages and unmaps them.
* The fast path is O(1), with no syscalls and no host allocation. Every failure
  returns a status and leaves the heap unchanged (tested under a budget).
* `MADV_NOHUGEPAGE` is set on actor mappings: with transparent huge pages a
  sparse actor heap faults in 2 MiB per touched page (measured: about 3x RSS
  at 10k actors).

## Measurements

The workload is one actor graph stored four ways. Each node is
`{ int64; ref; ref }`, linked to two earlier nodes, with one 4-byte (or
pointer-width) root per node. The table reports RSS delta per node. Each
variant runs in a fresh process; source is `tests/actor_slab_heap_bench.cpp`.

* `malloc` uses one `new` per node with native pointers, the common native
  baseline.
* `arena` uses a bump array with native pointers and no root table. It is an
  idealized lower bound, not a usable heap: no free, reuse or validation.

**aarch64 Linux (native, glibc 2.36, g++ 12), 1,000,000 nodes total**

| Actors × nodes | malloc | arena | Offset32 slab | Handle32 slab | Offset32 vs malloc |
| --- | --- | --- | --- | --- | --- |
| 1 × 1,000,000 | 40.2 B | 24.1 B | **20.4 B** | 28.4 B | **−49%** |
| 100 × 10,000 | 40.1 B | 24.2 B | **20.6 B** | 28.7 B | **−49%** |
| 10,000 × 100 | 40.4 B | 24.4 B | 48.6 B | 89.7 B | +20% |
| 100,000 × 10 | 44.1 B | 26.5 B | 449 B | 859 B | ×10 |

**32-bit, dense actors (Debian, g++ 12, emulated, so timings are meaningless)**

| Host | Actors × nodes | malloc (32-bit ptrs) | Offset32 slab | Change |
| --- | --- | --- | --- | --- |
| i686 | 1 × 1,000,000 | 36.3 B | 20.6 B | −43% |
| ARMv7 | 1 × 1,000,000 | 28.2 B | 20.5 B | −27% |

Allocation latency on aarch64: Offset32 averages 20–25 ns against 37–46 ns for
`malloc` in the same runs. The maximum (about 35 µs to 9 ms) is first touch or
commit of a new page, not the O(1) path.

### What the numbers say

* For actors holding a few hundred objects or more, Offset32 halves memory
  against per-object `malloc` with 64-bit pointers. That meets the proposed
  ≥20% gate with margin, and it also wins on 32-bit hosts.
* **Small actors lose.** Each actor pays at least one 4 KiB slab page per size
  class it uses, plus about 0.8 KiB of host bookkeeping, while `malloc` shares
  pages across actors. Break-even is a few hundred 16-byte objects per actor.
  This is the metadata/fragmentation risk the plan anticipated.
* Handle32 costs 8 bytes per object more than Offset32, so use it only where
  indirection is required.

## Next steps

1. **Small-actor floor.** Add a page-size profile (512 B–2 KiB pages for small
   actors, or a first slab sized from the compiler-known actor state size).
   Re-run the 10k/100k rows; the gate is "no worse than malloc at 100 objects
   per actor".
2. **Compiler integration.** Wire `rt take`, `rt borrow`, `rt share`, STOP,
   cancellation, panic cleanup and continuation frames to this allocator, and
   add compiled `.atto` allocation/deallocation fixtures for x86-64, i686 and
   ARMv7. Nothing here is compiled-guest evidence yet.
3. **CI gate.** Run the benchmark in CI and fail on regressions of B/node and
   reclaimed-after-STOP.
4. **Java/.NET parity.** Same words, statuses and teardown rules: Offset32 as
   the field word, and Handle32 profile selection per this document.
