# Native LLVM Share projection-root hardening V2

**Draft, source-authored. No actual source-org CTest, compiler, or guest execution.**

## Production checker hardening

The stage-one `OwnershipChecker.cpp` already rejects direct `rt share` on named borrowed variables and class fields. A **nested or computed field root**, however, was not consistently subjected to an owner-authority check before the field's read could be classified as a Copy scalar.

`NativeOwnershipPlace.hpp` now provides `tracked_share_projection_root`, bounded to eight named fields, refusing empty identifiers and computed, call, array, Borrow or BorrowMut roots. The real `Share` checker uses that classification for all field projections:
- Missing/untracked root: `E_SHARE_UNTRACKED_PROJECTION`.
- Compiler-tracked borrowed root: `E_SHARE_BORROW` before evaluating the scalar field.
- Owned named root: retain existing copyability and class/array field guards and the ordinary read path.

This is *source-provenance admission*, not general typed-MIR ownership, and a scalar copy may still be shared after an independently proven detachment.

## Additional test inventory

The new `tests/native_share_projection_root_test.cpp` contains **20 native AST classifier scenarios** across shallow/nested/bounded paths, missing/empty names, computed/call/borrow/array roots, and over-depth rejection. Production CMake registers `native_ownership_share_projection_root`. The exact-head LLVM workflow now requires **three** independent focused C++ CTests (tracked Take, borrowed Share, projection-root) and retains the existing **20** scoped compiler/guest CTests.

The nested projection behavior does **not** yet have a parser-positive, backend-executed `.atto` diagnostic receipt; this must be separately implemented and proven when typed nested-class lowering is available. No C++ host assertion counts as native compiler parity.

All previous zero-step Actions are runner-admission failures. Require nonzero source-owned steps, no CTest skips, actual compiler parser/semantic diagnostics, guest oracles where positive, independent review, and no private-code export.
