# AttoLang LLVM typed-place identity / real-source compiler matrix (v1)

**Status: DRAFT; the source-owned LLVM Actions jobs have not executed. This is a bounded HIR/CFG foundation, NOT complete typed MIR, NLL or five-backend proof.**

## Compiler-generated identities

- `native_ownership::BindingId` is assigned monotonically within each compiler-checked function for parameters, top-level locals, and scoped declarations. It is retained when `Locals` is cloned across CFG branches, and affine joins compare the typed identity, not just counts or variable spelling. ID zero is unminted and cannot resolve a source place.
- `native_ownership::OriginId` zero means **allocation origin not established**. Only the admitted `new_class` construction path mints a nonzero OriginId. A checked `move_class` transfers the existing origin rather than manufacturing a new one. Borrowed parameters and ordinary alias names never mint an independent heap-origin certificate.
- `ResolvedPlace` carries the lexical binding identity, optional proven allocation origin, bounded field path, and source owner/loan mode. Both `rt share`/`rt take` admission and the supported return-CFG read/move adapter use the same compiler-resolved place key.
- **Different BindingIds or OriginIds alone do not prove independent heap storage.** This implementation does not enable arbitrary disjoint mutable borrows, index moves, actor transport, interruption, or partial-move lowering beyond the existing admitted subset.

## 50 distinct real `.atto` compiler inputs

`tests/contracts/atto_ownership_place_mir_50.json` contains 50 unique complete source strings:
- **10 negative computed-source `rt take`** cases requiring `E_TAKE_UNTRACKED_PLACE`
- **10 negative borrowed-scalar `rt share`** cases requiring `E_SHARE_BORROW`
- **10 negative borrowed-class-field `rt share`** cases requiring `E_SHARE_BORROW`
- **10 positive owned-scalar `rt share`** cases requiring LLVM IR, Clang link and native guest exit 33–42
- **10 positive scalar `rt copy`** cases requiring LLVM IR, Clang link and native guest exit 33–42

These are candidate source fixtures, not compile receipts. The required source-owned `atto_ownership_place_mir_real_compiler_50` CTest invokes the actual `oreslang-llvmc` via the established `tests/contract.py` for **each** candidate, materializing a separate temporary `.atto` file. It rejects any missing/duplicated case, absent compiler or Clang binary, wrong semantic diagnostic, absent executable IR, failed link, wrong process exit, or timeout. Positive source text cannot pass with source-only compiler-shape checks. The matrix emits a terminal `ATTO_OWNERSHIP_REAL_COMPILER_FIXTURES=50/50_EXECUTED` only after all 50 subprocess-based checks pass.

`native_ownership_place_mir_manifest_17_mutations` is an independent HOST-ONLY Python unittest verifying the 50-input inventory and rejecting manipulated counts, duplicate source/IDs, forged statuses, disabled Clang execution, bad diagnostics and path traversal. It cannot substitute for the actual compiler CTest.

## Gates

The exact-head `atto-critical-ownership-llvm-parity.yml` must execute its previous four focused native CTests and twenty scoped compiler/guest cases, **plus** both new matrix tests. The workflow checks that the real matrix CTest is registered, runs with verbose captured output, requires the 50/50 terminal marker, and parses the one-CTest JUnit summary with zero skipped or failed cases.

The independent source-owner reviewer must verify immutable SHA and Git tree, actual executed steps, compiler identity and executable diagnostics, negative mutations, and parser-positive semantic evidence before merge. `runner_id=null` and `steps=null` remain `RUNNER_ADMISSION / NOT_EXECUTED`. No source is moved to another GitHub organization for testing.
