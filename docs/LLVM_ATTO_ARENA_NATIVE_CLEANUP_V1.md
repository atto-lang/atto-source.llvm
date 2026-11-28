# LLVM-local `.atto` arena cleanup — bounded native AOT bridge

**Status:** integrated candidate on LLVM PR #855; source-only / native execution
is **NOT_EXECUTED** unless independently verified from a stepful exact-head
build receipt. Not a general actor, continuation or exception implementation.

## Design

The public Atto source can stay in backend-owned `stdlib/core`,
`stdlib/alloc`, and `stdlib/std` .atto files. No exact byte identity with
Java, Forge, .NET, Wasm or other compiler implementations is required.
The `ScopedArena` source is an intentionally **uncompiled target surface**:
`OwnedArena`, `ArenaSlice` and `on_drop()` still require compiler-sealed
providers and true source-level lifetime checking.

The existing production native `ActorArena.c` and `ActorRegionV1.c` are
reused for the first **compiled class ownership subset**, not replaced:

1. Source is parsed and type/ownership checked by the real LLVM frontend.
   ActorRegionV2 accepts one closed class of 1–16 immutable literal int fields,
   1–8 fully initialized fresh lexical owners, one pure equality guard, and
   two normal return paths. No extra roots, owner moves, mutable borrow escapes,
   callback registration, await, throw, cancellation or dynamic dispatch.
2. The compiler snapshots the selected scalar return through a PHI and enters
   its shared cleanup block **exactly once**. It records the preverified reverse
   ownership order as an array of original native stack **sidecar pointers**.
3. One LLVM call to `ores_actor_region_v1_compiler_finish(region, turn, owners,
   owner_count)` authenticates original proofs, then invokes
   `ores_actor_arena_drop_owned_exact` under the already-existing arena lock.
   The preflight checks the precise complete live owner set, unique slot IDs,
   each generation/domain, loans, page claims, generation exhaustion and
   bound of eight. Any failed preflight invokes the native fail-stop boundary
   **before altering any slot**; it cannot drop an earlier owner and then
   discover that a later owner was borrowed or stale.
4. Only after all preflight conditions pass does the runtime scrub each slot,
   advance each generation, invalidate the original sidecars, leave the active
   turn, seal the region and physically destroy it. There is **no tracing GC,
   collector thread or hidden GC-dependent disposal** on this native path.
5. `retire()` may arrive while an admitted turn remains active. Ordinary
   `read`, `write`, and `alloc` still fail after retirement, but the bounded
   **cleanup-only** turn lease may complete destructor work; a later newly
   entered turn is not admitted. This is an ABI substrate, **not** proof of
   source-level async cancellation or actor supervisor integration.

The checked batch is *exactly* the live lexical owner set. It intentionally
rejects partial/moved/uninitialized owners; those need MIR per-owner drop flags
and a verified extended ABI before compiler admission. An explicit native
`ActorRegionV1` loan against any owner blocks batch destruction; the compiler
must never silently ignore the loan or extend a region through an untracked FFI
handle. Arbitrary guest code cannot directly mint the proof sidecar or raw
native pointers.

## Tests authored on the draft

- `bash tests/run_actor_region_compiler_finish_v1.sh` builds the **actual**
  two native runtime translation units and the new adversarial host harness with
  Clang and GCC × O0/O2 × ASan/UBSan. The harness exercises 800 successful
  exact-owner cleanup cycles (1–8 owners × 100 actor incarnations), half with
  admission sealed before cleanup, plus **14** POSIX fail-stop misuse cases.
- Existing `tests/actor_region_v2_multiowner_guest.cmake` now requires
  exactly one emitted checked-finish call and no per-owner legacy drop calls.
  It still compiles three real `.atto` guest programs with the actual LLVM
  compiler, screens IR for tracing GC, links Clang O0/O2 and checks exit codes;
  negative compiler admissions remain unchanged.
- `python3 -m unittest discover -s tests -p test_atto_arena_source_v1.py -v`
  checks the existing backend-local `.atto` source and 38 policy vectors.
  **These are source-only checks, not compiled AOT cleanup proof.**

## Release blockers

**Do not call this source release-ready** until the exact new branch head has
nonzero-step material native tests, compiler-backed guest tests and sanitizer
receipts, and the branch passes independent review. A failed GitHub Actions
job with `steps=null` is `NOT_EXECUTED`, not a native test failure or pass.

General cleanup still requires CFG/MIR initialized flags, nontrivial per-field
destructors, early-failure/exception unwind edges, explicit take!/copy!/borrow!
ownership, async cancellation and pinned continuations, actor stop coordination
with outstanding producer/FFI leases, and supported NativeAOT target proofs.
Fail closed if a compiler cannot prove one of those paths.

Avoid competing implementation duplication: common-source #13 tracks memory
conformance; LLVM PR #742 tracks elastic arena substrate; LLVM PRs #732–#734
cover the prior ActorRegion compiler/guarded cleanup chain. PR #855 extends
the mainline `ActorArena` and `ActorRegionV2Guest` integration, without
claiming ownership of the sibling proposals or changing their independent
source implementations. Private source must not be mirrored cross-organization
without the independently approved security gate.
