# AttoLang LLVM: compiler-resolved source places (bounded stage-one bridge)

**Status: draft, awaiting production C++/LLVM compiler and guest execution on source-owned CI. Not complete typed MIR or NLL.**

The native production `OwnershipChecker.cpp` previously classified ownership sources using AST names and separately constructed flat CFG keys from those names. This left a mismatch risk between the value checked before lowering and the storage place invalidated by the fixed-point `OwnershipCfg.hpp` verifier.

`NativeOwnershipPlace.hpp` now provides a compiler-scoped `ResolvedPlace` from the same `native_ownership::Locals` table used by the production checker. It captures:
- The resolved current-frame lexical slot and ordered field projections (maximum eight).
- Origin mode: `OwnedSlot`, `IncomingSharedLoan`, or `IncomingExclusiveLoan`; borrowed parameters never acquire owner authority from an i64 Copy type.
- One canonical `compiler_place_key` for flat CFG read/move keys. Root and projection names containing dots, undeclared roots, computed/untracked projections, and paths beyond the bound are rejected.

Production Share/Take admission and the return-only CFG adapter now use that resolved place before treating a field read as independently shareable or invalidating a take source. Take is still supported only for named owners/direct named fields; nested/unknown moves remain rejected. The full CFG verifier still checks possible move states, loan provenance, and alias-overlapping ancestor/child entry layouts.

`tests/native_compiler_place_origin_test.cpp` adds **50 native C++ scenarios**: 27 owned/shared-loan/exclusive-loan origin paths at depth 0..8, 18 forged missing/unnamed roots at depths 0..8, and 5 malformed/computed/overdeep cases. Each recognized path also checks the emitted canonical CFG key. This is a focused HOST_CPP17 compiler-metadata test, **not** a compiled AttoLang guest.

CMake adds the test target and the exact-head LLVM ownership workflow requires **four** focused native CTests plus the previously authored **20** scoped compiler/guest CTests, with no skipped/failed cases. Authoring code and the workflow is not execution proof.

**Limitations:** lexical slot identity alone does not establish independent heap allocation origins, disjoint class layout, task/actor send safety, exception cleanup, suspension safety, or nonlexical lifetimes. The next compiler stage must mint stable HIR/MIR origin IDs and prove root separation; no API here asserts that different lexical names imply independent storage.

**Merge gates:** source-org GitHub Actions with runner assigned and nonzero executed steps; actual production C++ build with GCC/Clang and LLVM dependencies; real parser-positive `.atto` semantic/guest receipts on immutable source head/tree; negative mutation evidence; independent ORESoftware review. Zero-step jobs remain NOT_EXECUTED. No cross-org private source export.
