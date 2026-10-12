# Native bounded `Array<int>` storage ABI (initial slice)

This is a **trusted native runtime substrate only**. The Oreslang frontend does
**not** yet parse/typecheck/lower `Array<int>`, class-owned array fields, or
the linked queue's `Option<QueueEntry>`/mutable method syntax. The native
compiler continues to reject all six Git-pinned queue fixtures with no partial
LLVM IR. Do not use these runtime tests as evidence of native source parity.

## ABI and limits

`NativeIntArrayRegion.h` exposes opaque owned array handles and only the
operations the queue's backing arrays need:

- `region_open` / `new` / `reserve` / `add`
- `length` / `get` / `set`
- `drop` / `region_close`

All values are signed 64-bit integers (the current native `int` ABI). An
array can hold **0–65,536** values. One owner region permits at most **64 live
arrays** and **262,144 reserved elements** across them. Geometric growth is
bounded and allocation/overflow checks precede metadata publication.

Every allocation receives a globally nonrepeating process-local token,
including after array-slot or region reuse, so stale handles cannot access new
storage. The token is **not** a cryptographic capability or authorization
credential. A handle is only valid with its original region while the slot
remains live. All element reads/writes are bounds-checked. No element pointer
escapes. Destroying an array zeroizes all reserved storage before freeing it.
Closing a region requires every owned array to have been dropped.

Invalid inputs, exhaustion, stale/cross-region handles, allocation failure
and region-close-with-live-owners abort deliberately. This is a low-level
compiler-only contract analogous to `NativeOwnedI64Region`, not a recoverable
guest `Result` API. The eventual lowering must supply error/unwind and drop
glue when the full Oreslang language semantics demand it.

## Ownership and concurrency

One owner invokes this ABI serially. It contains no mutexes and is **not**
safe for multi-threaded shared access; it neither serializes actor turns nor
implements reader/writer locking. The compiler must prevent shared writable
aliases and an owner move/drop while any loan is live. Actor-state queues
must still enforce the single-flight execution invariant independently.

## Runtime verification

`native_int_array_region_security` compiles the C ABI and runs its
Release-safe assertions. Positive tests cover two distinct arrays, signed
payload extremes, append/read/write, resizing, 65,536 entries, independent
ownership, slot reuse and cleanup.

Child-process adversarial tests require SIGABRT on out-of-bounds get/set,
oversized reserve, live-region close, duplicate drop, foreign-region access,
invalid tokens, exceeding the array count or element budget, stale token after
slot reuse, and append past the maximum. The test-only token allocator hook
also forces exhaustion and proves token reuse fails closed. The same exact
sources should run under Clang ASan+UBSan before merge.

## Required compiler follow-ups

1. Type/effect-aware parsing and source lowering for `Array<int>`, with
   allocation/free at compiler-selected cleanup points, and an explicit
   region value tied to the owning class/actor lifetime.
2. Class fields with owned array handles, transactional initialization,
   no field/reference escape, and deterministic destruction.
3. A checked `Option<int>` and `Option<QueueEntry>` representation,
   including allocation/return failure safety.
4. `mut self`, non-reentrant read/write methods, loops and full
   branch-sensitive dataflow for the queue.
5. **Same-source** Java and LLVM execution: all five positive fixtures,
   all negative ownership fixtures, differential reference traces,
   LLVM verification + Clang + sanitizers. Only then may the queue's
   fail-closed native gate be replaced with a parity gate.

The runtime C ABI is *not* a substitute for compiling the Oreslang queue.
