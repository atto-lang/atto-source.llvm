# Experimental native elastic actor heap v1

**Status: HOST_ONLY / NOT_EXECUTED_AS_ATTO_GUEST / NOT_CERTIFIED.**

This draft adds a separate C11 runtime substrate. It does **not** replace
`ActorArena.c`, `ActorOwnedI64Heap.c`, the existing hard-real-time prefault
allocator, the actor compiler ABI or the runtime scheduler. The compiler
still requires a separately proved actor heap lowering and guest execution.

## Admission and memory contract

- Each heap is opened with an actor ID, incarnation, hard logical byte limit
  (1..64 MiB), and bounded number of host-control slots (1..4096).
- First construction commits **zero guest payload bytes**. The control
  structure and slot table are allocated separately and **consume process
  memory not included in the guest-payload budget**.
- The first successful allocation obtains up to 4096 native payload bytes;
  later backing doubles on demand up to the actor's hard limit.
- Growth stages a new zero-initialized C allocation, copies existing live
  bytes, publishes new backing, **explicitly scrubs and frees** old backing,
  then refunds old backing to both tenant and process budgets.
- During relocation, **both old and full replacement bytes are charged**.
  Quota failure or injected allocation failure does not publish a new slab,
  change the actor cursor, change any handle, or leak a reservation.
- Shared ledger admission uses C11 atomic compare-exchange. Supplying the
  same ledger for tenant and process charges only once. Ledgers must be
  allocated and initialized by a trusted host before use.
- Guest ref32 `word=offset/8+1` remains logically stable across growth and
  reserves word zero for null. **It is not a capability.** Host access also
  validates domain, incarnation, *slot identity* and generation.
- Slot identity is required even when a retired handle's 32-bit word and
  generation collide with a live allocation in a different slot. Stale
  handles, foreign actor handles and double-drop are rejected.
- Last-owner drop scrubs the logical payload and resets the allocation
  cursor; remaining native backing is retained for the same actor until
  final host-controlled closure, which scrubs and frees it.
- The arena API returns failure on hard quota, tenant/process quota, invalid
  handles and native allocation failure instead of aborting its host process.

## Reproducible local native verification

```sh
bash tests/run_elastic_native_arena_v1.sh
```

The test runner requires **both GCC and Clang** and runs each at O1/O2 with
`-fsanitize=address,undefined -fno-omit-frame-pointer -Werror`, with leak
detection enabled. It verifies:

1. Two actor heaps use distinct domains even if both issue ref32 word 1.
2. Actor A grows from 4 to 8 KiB without changing either actor's live data.
3. B is denied when its proposed replacement would breach a shared
   transient-peak process/tenant quota; A remains readable.
4. A synthetic native allocation failure rolls back its entire pending
   reservation, and a subsequent growth works.
5. An 8 KiB final backing requires a 12 KiB physical-request peak when
   replacing a 4 KiB slab.
6. Old actor handles are denied after drop and a fresh owner sees zero data.
7. Even if a new slot matches a stale ref32 *word* and *generation*, the
   out-of-line slot identity prevents stale authorization.
8. Final actor closure scrubs native data and returns charged payload bytes.
9. Sixteen independently owned actors contend for a shared process/tenant quota; successful and denied growth preserve each actor's prior data and all accounting returns to zero.
10. The internal actor-domain ID counter saturates instead of wrapping and reissuing an older domain.

**Executed locally on 2026-11-10:** 4/4 compiler/optimization combinations
passed. Exact source Git blob SHAs were checked against GitHub:

- Header `04e5ec3f2c69610051e474a969e5ebb62e398ca8`
- Runtime `9549cd1c66f1615e077a9a556f111428981d0596`
- Test `3e647c26a03ce8bfd3f277281bf6c9d224de678f`

This is real native C host-code execution with sanitizers; **no compiled
AttoLang guest**, compiler lowering, release-AOT execution, source-org Actions
execution, native thread-race proof or seven-backend parity was established.

## Hard blockers before production

- Do not run guest callbacks, FFI or async continuations against this arena.
  The prototype has no lock and requires a **single scheduler-admitted actor
  turn** and explicit quiescence before close.
- Do not expose the arena pointer, budget pointers or full host handles
  directly to untrusted actors or tenant-provided native code. A real tenant
  boundary needs authenticated broker calls and independent isolation.
- Do not treat C `calloc` requested backing or atomic ledger charges as
  measured resident set size. Allocator metadata, control slots, stacks and
  engine overhead require separate process/tenant admission and RSS checks.
- No hard-real-time guarantee: `calloc`, free and relocation may fault or
  stall while an actor turn executes. The existing prefault RT path remains
  separate and must not be silently replaced.
- The current bump allocator may exhaust logical address space when long-lived
  objects coexist with many dropped non-tail objects. Reuse, compaction,
  offsets, alignment, ledger and actor closure need further tests.
- Verify independently that source-compiled actor `spawn`, message handlers,
  owned values, field accesses and retirement use this heap and that guest
  results satisfy the shared conformance fixture. Existing LLVM actor
  acceptance has separate unsupported/sealed-state restrictions.

Shared policy: [conformance #643](https://github.com/ores-truffle-oreslang/oreslang-platform-conformance/pull/643).

### Further native test hardening

On 2026-11-10 the native test was rerun under GCC and Clang at both O1 and O2 with ASan/UBSan after adding 16-thread budget contention and saturating domain-ID admission. 4/4 configurations passed locally with `-pthread`. These tests establish only the host C allocator's behavior; they do not constitute ThreadSanitizer certification or multi-threaded access to *one* arena, which remains prohibited.
