# Native bounded `Array<int>` ABI and lexical source subset

The trusted native storage ABI is independent of the source language.
LLVM PR #267 adds the **first compiler-generated** synchronous,
nonescaping `Array<int>` subset. The supported source form is:

```oreslang
pub fnc main(): int {
  let mut Array<int> a = new Array<int>();
  a.add(41);
  a.set(0, a.get(0) + 1);
  return a.get(0); // 42
}
```

Only lexical `let mut Array<int>` owners are admitted, with `add(value)`,
`get(index)`, `set(index,value)` and `.size`. Their region is private to
the synchronous function; array handles are opaque i64 tokens used only for
trusted ABI calls. A zero-argument `new Array<int>()` allocates a fresh array.
Arrays are dropped in reverse lexical order on ordinary/guarded returns,
followed by region close. No guest reference or element pointer escapes.

**Not yet supported:** array arguments, move/copy/borrow/return, class or
actor fields, async/suspension, user exception/unwind, array methods other than
the admitted four, dynamic array construction in branches, `Option` and the
full linked-queue grammar. Out-of-bounds/alloc/quota failures currently abort
the process, not return an Oreslang `Result`. The six Git-pinned exact queue
fixtures remain intentionally rejected without partial LLVM IR. Passing this
first source gate must not be described as linked-queue parity.

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

## Source-level verification

`native_int_array_source_execution` compiles real `.ores` files with
`oreslang-llvmc`, checks that the compiler-generated LLVM IR contains the
owned array calls, links that IR to the trusted C ABI with Clang, then
executes the native program. It includes multiple array owners and early
return cleanup. Negative cases require a targeted ownership diagnostic,
compiler exit 1, and **no LLVM IR on stdout**. The separate
`native_int_array_source_asan_ubsan` gate runs the same programs under
Clang sanitizers.

The pre-existing `native_int_array_llvm_abi` test instead uses
**handwritten** LLVM IR. Passing it alone does not establish source
lowering. The same pinned sources are mirrored onto a funded private runner
for real LLVM 18 compilation when repository Actions minutes are exhausted.

## Required compiler follow-ups

1. Class-owned array handles, region lifetime tied to owning actor/class,
   transactional multi-field initialization, no alias escape, correct cleanup.
2. A checked `Option<int>` and `Option<QueueEntry>` representation,
   including allocation/return failure safety.
3. `mut self`, reader/writer safety, loops, return-guard purity and full
   branch-sensitive dataflow and exceptional cleanup for the exact queue.
4. **Same-source** Java and LLVM execution: all positive fixtures,
   negative ownership fixtures, differential reference traces,
   LLVM verification + Clang + sanitizers. Only then replace the
   queue's fail-closed native gate with actual parity tests.

The lexical compiler slice is *not* a substitute for compiling the queue.
