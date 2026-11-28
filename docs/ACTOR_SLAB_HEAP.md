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

## Width policy: what is 32-bit, and what must not be

32 bits is the right width for **references inside one actor's heap**, which is
where reference memory goes. It is the wrong width for anything that counts
monotonically, crosses actors or processes, or must be unguessable.

| Thing | Width | Why |
| --- | --- | --- |
| Actor-local object reference (field, array element, frame slot) | **32-bit Offset32** | Up to 32 GiB per actor heap with the 8-byte quantum; halves edge storage |
| Stale-protected reference (Handle32) | **32-bit**, with Handle64 as a profile | Slot bits are capacity-sized and slots quarantine on exhaustion. A hot, high-churn, long-lived actor that would quarantine its table should select a 64-bit handle (32-bit slot + 32-bit generation) |
| Dense row / ECS index | **32-bit** | Same reasoning as Offset32 |
| Allocation generations, heap and actor incarnation IDs, epochs | **64-bit** | A 32-bit counter wraps in minutes at 10M/s, and a wrapped generation re-admits ABA |
| Mailbox, sequence and ticket counters | **64-bit** | Same wrap argument. `BoundedMpscQueue` was fixed to `uint64_t` for exactly this |
| Byte budgets and stats summed across actors or the process | **64-bit** | Totals routinely exceed 4 GiB |
| Actor heap larger than 32 GiB | **native64 mode** | Explicit opt-in (`force_native64`), never a silent widening |
| FFI, OS, MMIO and code pointers | **native width** | Never compressed; explicit conversion only |
| Cross-actor references (`ActorRef<T>`) | **64-bit or wider capability** | Needs actor identity plus incarnation, not a heap offset |
| Durable or distributed identities (persisted actors, cross-node) | **128-bit** | Collision-free without coordination (UUID-class) |
| Unforgeable tokens (tenant capabilities, wire secrets) | **≥128-bit random** | Must resist guessing; 32/64-bit tokens are brute-forceable |
| Guest integer values (`int`, i64) | **as declared** | Values, not references: never narrowed by this policy |

Rule of thumb: **compress what is *stored many times*, not what *counts or
identifies*.** A 4-byte reference saves memory per edge. A 4-byte counter or
identity only saves 4 bytes once, and turns wrap-around into a security bug.

## Small-actor profile (November 2026)

`BasicActorSlabHeap<PageBytes>` and `BasicActorHeapRegionPool<PageBytes>` take the
slab page size as a profile. `ActorSlabHeap`/`ActorHeapRegionPool` stay at 4 KiB,
which is the layout the golden fixtures pin. `ActorSlabHeapSmall`/
`ActorHeapRegionPoolSmall` use 1 KiB pages; size classes above 960 B take the
large path.

**Why the page size alone was not enough.** In a shared pool an actor's RSS is
about `min(slot size, OS page)`, because every slot's first page is touched. Small
pages only save memory when slots are small enough to share OS pages, so tiny
actors need a **tight reservation** (no slack pages) as well. The pool now sizes
slots in the heap's page profile, and on release it only `MADV_DONTNEED`s OS pages
that lie entirely inside the slot.

Measured RSS per node (aarch64 Linux, `actor_slab_heap_bench`, 16-byte nodes):

| Actors × nodes | malloc + 64-bit ptrs | 4 KiB, 3 slack pages | 1 KiB, tight (0 slack) |
| --- | --- | --- | --- |
| 10,000 × 100 | 40.6 B | 48.8 B | **28.4 B (−30% vs malloc)** |
| 100,000 × 10 | 44.2 B | 449 B | 142 B |

Guidance: use the 4 KiB profile for actors that grow (its headers are 1.6% of a
page against 6.3% for 1 KiB). Use the small profile with a tight reservation for
fleets of small actors. At about 10 objects per actor the remaining floor (about
1.4 KB) is host bookkeeping: the heap object, the page-kind table and the minimum
1 KiB page. Shrinking that, for example by embedding the page-kind table in the
first page, is the next step.
