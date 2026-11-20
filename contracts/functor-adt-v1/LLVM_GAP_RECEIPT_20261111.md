# LLVM nested functor / ADT test admission — 2026-11-11

**This is an observation, not LLVM functor parity.** The [exact-source private Actions run #42966975809](https://github.com/numerics-atto/mirror-oreslang-source.llvm/actions/runs/42966975809) built the real `oreslang-llvmc` from the source PR's full `src/main` Git subtree and executed the compiler admission test on all 17 byte-identical `.atto` programs.

## Exact verified scope

- LLVM PR [#1686](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/1686), source `src/main` subtree `f38620ecc06f1b6cb28639533139d1fbbe87244c` (250 files), 17 guest fixture subtree `be201b1c667057582a6e7e53025bb1d1938179c6`.
- Private carrier commit `5d2530c8bddccbe3fa1cf4f6812f4ff048396ea5`. Pinned exact CMake, `compile-aot.sh`, Rust IR-audit script and test runners; **not** an entire-source-repository mirror. No cross-organization secrets or tokens used.
- Node reference model: **12 tests PASS**, zero failures. This model does not execute guest code.
- Actual LLVM compiler: **17 fixtures attempted, 17 nonzero exits**; **2 positives not executed**; **15 negatives rejected**, but **0 semantically certified**.
- LLVM O0/O2 native guest functor executions: **0**, since neither positive passed source admission. ASan/UBSan and owner/destructor evidence **not executed**.

## Actual compiler diagnostics

Fifteen fixtures, comprising both positive guests and 13 of the negatives, stopped at:

`oreslang-llvmc: native sum class/compound payload requires ownership and lifetime lowering`

Two other negatives stopped earlier at unrelated rules:
- `bad-map-err-domain.atto`: `oreslang-llvmc: expected ')'` (parsing boundary)
- `bad-unresolved-transpose.atto`: `oreslang-llvmc: scalar local requires an explicit ': int' or ': bool' type` (local annotation admission)

The evidence must **not** be reported as 15 successful LLVM negative type tests: the intended functor-specific checks are not reached.

## Required implementation follow-up

1. Admit nested Option/Result type arguments and owned sum payloads at the LLVM frontend and typed IR, with explicit copy/move/borrow lifetimes.
2. Implement `map`, `map_err`, `and_then`, both `transpose` directions and precisely one-level `flatten`, preserving nominal error identity.
3. Compile both existing positive guest programs into actual native IR and run O0/O2 binaries with exact observable outputs, no tracing-GC IR and actor-owned drop accounting.
4. Replace 15 generic/early rejections with per-fixture **expected semantic diagnostic categories**, including borrowed affine escape and wrong invariant nested return types.
5. Add ASan/UBSan and destructor-count checks for every constructor path, only promoting LLVM parity after these gates actually pass.

Source-free pass, compiler admission pass and actual guest execution remain separate evidence levels.
