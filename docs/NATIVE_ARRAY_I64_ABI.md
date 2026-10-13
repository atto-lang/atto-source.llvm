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

PR #270 extends this restricted subset to **class-owned arrays**:

```oreslang
define class Shelf as
  pub let int id = 7;
  let mut Array<int> items = new Array<int>();
end

pub fnc main(): int {
  let mut Shelf shelf = new Shelf();
  shelf.items.add(42);
  return shelf.items.get(0);
}
```

Class fields are prepared in source order, then their addresses are published
to the function's checked local symbol table only after initialization.
Array handles are dropped in reverse field order, before the enclosing class
stack lifetime ends. They share a function-private region with any standalone
owned arrays. Static maximum: 64 owned arrays across all constructed locals and
class field slots in one function, and 16 fields per class.

**Critical limitation:** the native array allocator aborts on failure.
"Prepared before publication" is **not** a recoverable atomic transaction:
there is no guest-visible rollback on OOM, quota exhaustion, exception or
unwind; those must be implemented before claiming failure-atomic class
initialization. No partial instance becomes visible on abort because
construction is synchronous with no escape and the process terminates.

**Not yet supported:** array or array-class arguments, move/copy/borrow/return
of owned array-bearing classes, class field handle replacement, actor fields,
async/suspension, user exception/unwind, methods on classes, array methods
other than the admitted four, dynamic array construction in branches,
`Option` and the full linked-queue grammar. Out-of-bounds/alloc/quota failures currently abort
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
Growth likewise allocates a fresh zero-filled buffer, copies live elements,
and scrubs the entire old capacity **before** freeing the old buffer (rather
than relying on `realloc`, which may release the old block without wiping).
A failed growth leaves existing values, length and reservation metadata
unchanged. The test-only backing-storage scrub counter verifies each growth
and final destruction, separately from metadata wiping.

Every live-handle operation first validates the internal slot invariants:
`length <= capacity <= 65,536`, the capacity fits the region reservation
accounting, and backing storage is present exactly when capacity is nonzero.
This includes no-op `try_reserve`, `try_add` without growth, reads, writes
and destruction; corrupt metadata triggers a process abort rather than being
treated as quota exhaustion or successful access. Recycled slots must have
fully cleared metadata. Test-only corrupted-length injection verifies the
fail-closed behavior in child processes on each access path; it is unavailable
in production runtime builds.

Owner-creation, reservation, destruction, bulk staging and region close also
reconcile the aggregate `live_count` and `reserved_elements` counters with
all 64 actual slot records. Inactive slots must have zero tokens, capacities,
lengths **and** null backing pointers. Inconsistent accounting aborts before
any new handle is issued, storage is resized/dropped, or a region is freed.
This is especially important on `region_close`: trusting falsified zero
counters would otherwise abandon live backing allocations without zeroizing
them. The bounded scan is not performed on hot-path element reads/writes.
Seven test-only poisoned-counter death tests exercise these boundaries.
Closing a region requires every owned array to have been dropped.

Invalid inputs, exhaustion, stale/cross-region handles, allocation failure
and region-close-with-live-owners abort deliberately. This is a low-level
compiler-only contract analogous to `NativeOwnedI64Region`, not a recoverable
guest `Result` API.

The LLVM compiler now retains the *exact checked status* in a compiler-owned
i32 slot on every native region-open, owner-create and `Array<int>.add`
resource-failure edge. Only `LIMIT=1` and `NO_MEMORY=2` are admitted into
their separately named post-cleanup terminal blocks; an unexpected status
also terminates. The status is saved **before** the shared reverse-order
owner destruction and region closure, so later typed error propagation
cannot confuse the cause with cleanup effects. All three terminal blocks
currently `abort`: returning normally would expose destroyed owners.
The compiler's validated LIMIT and NO_MEMORY blocks now call a dedicated
`oreslang_int_array_unhandled_resource_failure(uint32_t status)` ABI
**after** reverse owner drops and region close; the failed region-open path
also calls it after independent region cleanup. The ABI is allocation-free,
non-returning and accepts only the two defined recoverable resource status
tags, rejecting any other tag as an invariant failure. This provides a
typed, value-only terminal error handoff that native test wrappers can
observe without touching guest-owned data. It is not a catch frame or
supervisor mailbox delivery: the current runtime still aborts, and no guest
frame resumes. Tests verify exact LIMIT vs NO_MEMORY identity and zero
double-drop/leaks *before* the terminal handoff.

These status blocks are **not** yet source-visible `throw`, `trap`,
`Option` or `Result` semantics. The eventual lowering must supply a
well-defined typed guest exception/unwind handler and error-payload lifetime,
and must not accidentally catch `raise` or `panic`.

The eventual lowering must supply error/unwind and drop
glue when the full Oreslang language semantics demand it.

## Recoverable internal resource API and staging contract

The trusted runtime now also exposes a **checked internal ABI** while keeping
all previously compiled `oreslang_int_array_*` entry points and their
abort-on-error semantics unchanged. `ORES_NATIVE_ARRAY_OK`,
`ORES_NATIVE_ARRAY_LIMIT`, and `ORES_NATIVE_ARRAY_NO_MEMORY` distinguish
ordinary resource exhaustion from ownership corruption.

- `try_region_open(out)` and `try_new(region, out)` never write their
  output place when allocation or slot admission fails.
- `try_reserve(region, handle, capacity)` and
  `try_add(region, handle, value)` leave length, previously committed
  elements, and region reservations unchanged on failure. When geometric
  growth for `try_add` would exceed the aggregate region budget but one
  additional element fits, it retries with the exact minimum required
  capacity. Failed fallback allocation leaves previously committed data
  unchanged. No-op reserve on an internally consistent live owner is always successful. Over-limit size/count/budget returns `LIMIT`;
  exhausted native memory returns `NO_MEMORY`.
- `try_prepare_many(region, n, out)` acquires up to 64 independent empty
  arrays without publishing any output handle until all acquisitions pass.
  On any failure, it drops all staged handles in reverse acquisition order
  and preserves all pre-existing region arrays.
- `try_prepare_filled_many(region, n, lengths, initial_values, out)`
  atomically stages up to 64 fully populated arrays with exactly the requested
  lengths and default values. It checks all declared sizes and their aggregate
  reservation cost against the region budget **before** consuming a handle
  or allocating backing storage. Every slot or backing-allocation failure
  drops previously staged owners in reverse order, zeroizes their filled data,
  leaves existing published arrays untouched, and does not modify the caller's
  output handle slots. Filling uses exact-length reservations, not repeated
  geometric append allocations.
- `commit_many(region, n, destinations, sources)` validates all source
  tokens, destination tokens, uniqueness and pointer aliases **before**
  performing any mutation. On success, it drops old destination arrays,
  moves staged tokens to the destination owner slots, and disarms source slots
  with zero. It performs no allocation and has no recoverable error exit.

**Safety contract:** invalid/stale/foreign handles, duplicate mutable owner
places and token exhaustion still abort, including in `try_*` functions.
`commit_many` is a trusted compiler primitive, not callable from guest
Oreslang. Source-level array assignment/move is **still rejected** until
the compiler emits this transfer only after proving exclusive ownership.

The test-only `ORESLANG_NATIVE_TEST_ALLOC_FAILURE` flag enables
`test_fail_after(n)` to deterministically inject a resource failure at
region allocation, logical slot creation or backing-array growth. This hook
must never appear in a production runtime. The dedicated
`native_int_array_transaction_recovery` CTest verifies fault injection,
prepared-owner rollback, old-value preservation, full 64-slot recovery,
atomic owner transfer, quota exhaustion, and adversarial alias/stale-token
rejections. The filled-array audit injects failures at **all eight** slot and
backing-storage allocation edges for a four-array queue initialization, then
verifies old values, untouched output slots, full aggregate quota recovery,
and an allocation-free four-owner commit. LLVM guest-source, exceptional unwind, recoverable `Result`
conversion, and multi-field class replacement lowering are **not** implemented
by this substrate change.

## LLVM checked allocation source lowering (incremental)

The native LLVM code generator now lowers the already-admitted synchronous,
non-escaping lexical and class-owned `Array<int>` subset using the checked
runtime resource API:

- `try_region_open(&region)` for the private owner region;
- `try_new(region, &handle)` for each owned local or class field;
- `try_add(region, handle, value)` for dynamic array growth.

Each status is compared with `ORES_NATIVE_ARRAY_OK`. No checked failure
continues into guest code or reads an uninitialized handle. Per-function
shadow handle slots start at zero *before* any allocation. Every successful
creation records its token in its own shadow, independently of class-field
publication. If a resource operation fails, generated LLVM IR drops every
acquired handle in reverse order, closes the array region, cleans any live
native owned-i64 local regions, and then aborts. The failed region-open path
does not attempt to close a nonexistent region.

**Important contract boundary:** This is **not** guest-visible recoverable
`Result`, `throw`, `trap`, or supervisor handling. The code generator
observes the checked result, cleans up, then intentionally aborts as an
interim fail-closed process behavior. Until typed propagation and unwind are
implemented, callers cannot recover from an array allocation error.
Bounds/get/set checks continue to abort in the original runtime.

The Linux-only `native_checked_array_source_failure_cleanup` test verifies
the actual emitted ABI calls and injects deterministic faults at every region,
owner creation, and growth allocation point for three source-compiled guest
programs: two lexical arrays, four class-owned arrays, and mixed scalar
`owned int` plus `Array<int>`. ELF link wrappers assert the exact numbers
of both array and scalar owner drops and both region closes **before**
compiler-generated abort; normal execution asserts exact-once cleanup and
a successful result. A parallel ASan+UBSan gate uses the same
compiler-produced IR. No hand-written surrogate IR is accepted.
The mixed-owned-int fixture is explicitly gated with
`--experimental-owned-int` and is not enabled by default.

This lowering is still restricted to the earlier lexical/class subset; it
does not implement `self.field = staged`, native `Option`, queue class methods,
loops, cross-region replacement, asynchronous suspend, or exceptional unwind.
All six strict exact linked-queue source rejection gates remain.

## Verified whole-class owned-array transfer

The native frontend now admits **only a whole-class** `rt take(original)`
for a live unborrowed class that owns `Array<int>` fields:

```oreslang
let mut Shelf original = new Shelf();
original.items.add(42);
let mut Shelf moved = rt take(original);
return moved.items.get(0);
```

The ownership checker requires matching class types, a live unique source,
no partial field move or active loan, and a mutable destination owner. LLVM
lowering transfers each opaque array token into the destination class field
and clears the old field to zero. A moved source cannot be accessed again.
At normal return, conditional class-field cleanup skips zero/disarmed owners,
then drops each live destination token exactly once. On an array resource
failure after the move, the pre-existing allocation shadow table still owns
exactly the original tokens and the shared failure cleanup drops each one
once without consulting possibly disarmed source field storage.

This is **not** field-wise reassignment, cloning, array handle return,
cross-call class passing, shared aliases or `commit_many` lowering. The
existing restrictions on array class parameters, mutable field assignments,
nested borrows and actor-owned arrays remain fail closed. The transaction
test includes whole-class move chains, guarded returns, fresh later
allocations, stale source/duplicate move/immutable destination rejection,
and normal plus ASan+UBSan fault injection at every allocation point.

## Restricted direct Array<int> field ownership moves

A synchronous, straight-line native function may transfer a live lexical
`let mut Array<int>` owner into an exclusively held mutable class field:

```oreslang
let mut Shelf shelf = new Shelf();
let mut Array<int> staged = new Array<int>();
staged.add(42);
shelf.first = rt take(staged);
return shelf.first.get(0);
```

This is an **infallible owner-handle transfer**, not a copy.
The compiler verifies a live, uniquely mutable class destination and
a direct named, live, unborrowed lexical `Array<int>` source. It drops
the superseded destination owner, zeroes its original failure-cleanup
shadow, transfers the incoming opaque handle and disarms the lexical
source slot. The incoming array's original shadow remains the cleanup
authority if any **later** checked resource allocation fails; normal
exit drops each current owner exactly once. Shadow provenance is also
propagated across whole-class `rt take` transfers.

Implicit copies, scalar sources, second moves, stale lexical reads,
borrowed destinations and conditional transfers are rejected before IR.
Branch-dependent ownership phi slots are not yet implemented; this
first field-move slice is restricted to straight-line code, with
deferred field moves remaining unsupported. Four sequential field
assignments are **not** an atomic four-owner transaction. Full linked
queue initialization still requires batch preparation, a single
`commit_many` and publishing `ready` only after successful commit.

Source-compiled Linux regression tests cover repeated field replacement,
replacements on previously moved classes and precise no-IR negatives.
Deterministic allocation failures *after* a replacement must destroy the
superseded handle and remaining live owners exactly once; Clang
ASan+UBSan runs the same compiler-generated guest programs.

## Native/runtime linker symbol admission

Compiler-owned C runtime imports and generated exports must not share an
unqualified ELF/Wasm linker symbol with a source function. LLVM
`Function::Create` can otherwise silently rename the guest function to a
numbered suffix such as `abort.1`, violating the native source-to-symbol
identity contract. The frontend now rejects unqualified source functions
named `abort` or `__oreslang_guest_main`, or starting with
`oreslang_int_array_`, `oreslang_owned_i64_region_`,
`ores_actor_i64_heap_`, `oreslang_source_actor_`,
`oreslang_actor_`, or `oreslang_wasm_actor_`. Existing
`oreslang_int_array_` rejection is retained regardless of module name.
Other names are checked when they would generate an unqualified linker
symbol. Module-qualified user functions have distinct symbol names.

The native class array source gate asserts seven additional forbidden
symbol families are rejected **without IR**, rather than inspecting
possibly auto-renamed generated functions. The same gate also constructs
64 owned arrays across four classes and transfers all four complete
classes without allocating any further handle. All sixty-four original
handles are subsequently destroyed exactly once.

This is a source/linker ABI namespace invariant, **not** guest-level
exception handling, typed array allocation recovery or actor supervisor
delivery. The checked resource failure cleanup continues to use trusted
`abort` after exact-once destruction until typed error propagation is
implemented.

## Ownership and concurrency

One owner invokes this ABI serially. It contains no mutexes and is **not**
safe for multi-threaded shared access; it neither serializes actor turns nor
implements reader/writer locking. A separate native CTest creates and drops
arrays concurrently in independent thread-owned regions, then verifies all
issued process-global tokens are unique; this does not admit shared access
to one region. The compiler must prevent shared writable
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

## Field-place and declared-effect audit invariants

The native frontend must preserve the complete syntactic receiver place until
the ownership verifier resolves it. A standalone scalar/class-field assignment
is restricted to a **direct named class receiver**, never a nested path:
`box.items.size = 7` fails admission, rather than being rewritten into
`items.size = 7` on an unrelated local named `items`. Similarly,
`box.items.send(7)` cannot be interpreted as `items.send(7)` on an
unrelated ActorRef. Direct `worker.send(7)` retains normal actor lowering.

`rt take(box.items.size)` is not a move of a directly addressable class field
and fails closed; simple reads such as `box.items.size` and
`box.items.get(0)` remain legal. These are distinct from ownership-handle
access, which must never be treated as a scalar int.

If a function has an explicit `using` effect signature and constructs a
class containing `Array<int>` fields, it must declare `alloc` even when
the function itself never calls an array method. The constructor is an
allocation operation, not a pure stack-only class initialization. This check
does not certify the transitive effects of guest exception/abort paths;
recoverable error handling remains unsupported.

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

1. Complete class/actor-lifetime regions and *recoverable* transactional
   initialization with rollback/drop on allocation or exceptional failure,
   plus proven non-copying owned-field transfer and branch sensitivity.
2. A checked `Option<int>` and `Option<QueueEntry>` representation,
   including allocation/return failure safety.
3. `mut self`, reader/writer safety, loops, return-guard purity and full
   branch-sensitive dataflow and exceptional cleanup for the exact queue.
4. **Same-source** Java and LLVM execution: all positive fixtures,
   negative ownership fixtures, differential reference traces,
   LLVM verification + Clang + sanitizers. Only then replace the
   queue's fail-closed native gate with actual parity tests.

The lexical/class-field compiler slices are *not* substitutes for compiling
the exact linked queue. Its existing six strict fail-closed fixtures remain.
