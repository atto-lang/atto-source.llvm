# Inverted class handle / receiver reference contract (test-only)

This is a **C++17 host-side reference oracle**, not executable AttoLang
`.atto` class lowering, a native compiler ABI, a library delivered to guests,
or the production actor-owned heap/stack runtime. The implementation lives
under `tests/support` deliberately; it must not be linked as a runtime
substitute.

## Invariants modeled

- A handle has a private issuer, actor identity, 32-bit slot, and 32-bit
  generation. The user cannot publicly construct a forged numeric handle.
  This **does not authenticate the caller's current actor**: a trusted C++
  host caller holding the owner reference and valid handle can access it from
  another thread. Real guest compilation must obtain the acting actor from
  a trusted runtime/scheduler context and enforce that identity before access.
- The issuer is unique per owner instance, including when the same actor ID
  and memory address are reused; a stale handle cannot pass an ABA reuse check.
  The issuer counter is **shared across different capacity/schema template
  instantiations** rather than accidentally restarting at 1 for each type;
  invalid actor IDs are rejected before consuming a new issuer.
  The contract test checks issuer continuity when constructing both an
  eight-row and two-row table and confirms failed zero-actor construction
  does not advance the sequence.
- Dense physical row movement on swap-delete does **not** change the identity
  of surviving rows. Deleted slots increment generation before reuse.
- Generation exhaustion retires a slot permanently rather than wrapping.
- `snapshot(handle)` returns a detached two-i64 value; owner mutations,
  deletion and closure cannot change the captured result.
- `move_by(handle, dx, dy)` is one receiver-style atomic operation: owner lock
  held from identity validation through both checked additions and updates.
  Overflow rejects both writes, never leaving only one field updated.
- Checked readers racing a closing owner either complete before closure or
  receive a closed-owner error; underflow as well as overflow rolls back
  the entire two-field receiver update.
- Multiple receivers may execute on independent host threads. The reference
  oracle serializes each operation with a mutex. This does **not** implement
  or prove AttoLang's non-parking actor scheduler, sealed state discipline,
  hard-real-time behavior or isolated logical stacks.
- The table uses fixed-size inline arrays, without dynamic storage or tracing
  GC in its own implementation. It does **not** establish a general zero-GC
  linked binary, actor arena allocation, or a compiler-level ownership proof.
- Operations fail on owner closure. As with all C++ objects, callers must
  externally synchronize destruction against concurrent use.

## Locally executed host-only regression

The two committed C++ files were compiled and run using GCC/Clang with
`-std=c++17 -Wall -Wextra -Wpedantic -Werror -pthread`:

- GCC `-O2`: passed
- Clang `-O2`: passed
- GCC `-O1 -fsanitize=address -fno-omit-frame-pointer`: passed
- GCC `-O1 -fsanitize=undefined -fno-omit-frame-pointer`: passed
- GCC `-O1 -fsanitize=thread -fno-omit-frame-pointer`: passed
- Clang `-O0` and `-O2`: passed (both optimization levels)
- GCC `-O0`: passed
- Clang `-O1 -fsanitize=address`: passed
- Clang `-O1 -fsanitize=undefined`: passed

### Model-based differential hardening

`tests/inverted_class_handle_fuzz_test.cpp` executes 50,000 deterministic
insertions, checked receiver updates, swap-deletes, snapshots, recycled-slot
lookups and stale-handle denials against an independent value/identity model.
All live handles are rechecked after each operation, and every retired handle
is denied at the end. The exact committed test source was verified with
`git hash-object` as `43faa90947efce5fdc47501ff679661070c9f8ff`.

Executed locally: GCC/Clang `-O0`/`-O2` and GCC ASan/UBSan/TSan
(7 executions), all passing. The GitHub Actions workflow now requires both
the fixed regression and model-based differential for each compiler and
sanitizer matrix entry; workflow runs must have real executed job steps to
claim independent CI success.

After the shared-issuer hardening, I recompiled and executed both exact
C++ files locally: GCC/Clang `-O0/-O2` plus GCC ASan/UBSan/TSan,
**14 passing executions** (seven configurations per test). The source
header blob is `66b8e2eaa4d9d3307df0a5fef7286b7c5c8dd5dd`; the
updated fixed regression test is `c012733ec2c0f63a2dffa2b6611f50b00de05490`.

This receipt covers a test-only oracle. It is not `oreslang-llvmc` AOT testing.
The source-owned GitHub workflow will provide separately pinned source-tree,
compiler/optimization and sanitizer receipts when Actions jobs actually run.

## Next compiler stage (not implemented here)

1. Define an independent inverted **class** AST and typed MIR row handle,
   distinct from value-struct snapshots. Retain rejection of class declarations
   in the existing production LLVM parser until verified.
2. Compile opaque slot+generation/issuer/actor tokens into 32-bit arena-relative
   actor-owned metadata without a raw guest pointer, leaking references or GC.
3. Lower `mtd (mut self: Particle) move_by(...)` through an atomic receiver
   transaction, with checked arithmetic and exception-safe rollback.
4. Prove stale, foreign, closed-owner, cross-actor and recycled-slot references
   trap before memory access, including concurrency and hot-load invalidation.
5. Run actual `.atto` source -> independent HIR/MIR -> LLVM IR -> Clang native
   AOT `-O0/-O2`, GCC/Clang ASan/UBSan/TSan where applicable, and cross-backend
   comparison. Do not claim complete inverted class semantics until then.

The source CI security gate is managed independently in
[LLVM issue #1804](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/issues/1804).
No private source Git tree was copied to an alternate GitHub organization.
