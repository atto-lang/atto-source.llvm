# Interior free-range reuse in the fixed actor-owned byte heap

The bounded `ActorOwnedHeap<Bytes, Slots>` has one authoritative actor-local
byte array and fixed-size slot/generation metadata. This draft adds an
`std::array<Hole, Slots + 1>` with monotonically ordered, coalesced free
extents. No per-message `malloc`, reference counting, tracing GC, or
heap-owned linked-list node is required for slot recycling.

Allocation first searches interior holes, then extends the bump cursor.
The complete physical extent **including alignment padding** belongs to the
allocation until deterministic drop. Drop scrubs its whole extent and then
either rewinds the bump tail or inserts and coalesces an interior free range.
Holes contiguous with the bump tail are automatically consumed. No live
allocation is relocated and the backing array is never compacted.

All public handles still carry the heap identity, actor incarnation, slot,
and monotonically advancing generation. Interior byte-offset reuse never
authorizes a stale slot handle or a raw four-byte compact offset without its
original out-of-line provenance proof.

## Proof limits

- This is a fixed-capacity, single-authority native runtime substrate,
  not compiler-generated `rt take` or automatic lexical drop lowering.
- Source-level actor turn exclusivity, `await` continuations, FFI borrows,
  authenticated retirement proof and scheduling admission remain separate.
- The first-fit allocator may fragment; it fails closed if no **contiguous**
  region fits despite sufficient aggregate free bytes.
- `std::array` state belongs to the actor host C++ object; it is not a
  per-message managed allocation or evidence about Java/.NET GC behavior.
- The companion native C++ tests cover adjacent-hole coalescing, alignment
  and capacity churn, zero-filled reuse, stale generational handles and
  persistent live-object preservation.
- Full exact-head builds (GCC/Clang, x86-64/ARM64, sanitizers) and reviewer
  verification remain required before merging. The independent offline
  stress model is not a substitute for running the private repository tests.

This PR depends on the tail-rewind draft #843; the separate heap-ID overflow
draft #838 touches the same actor byte heap header and must be reconciled
before either change is release-ready.
