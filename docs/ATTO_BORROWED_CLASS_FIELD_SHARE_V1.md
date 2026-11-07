# Native LLVM borrowed field Share provenance — stage-one hardening

**Compiler fix in the real `OwnershipChecker.cpp`, with separate C++ classifier and .atto compiler-negative tests.**

Previously, the native `rt share` path rejected directly named borrowed scalar variables but did **not** check the `Parameter::Mode` of the root owner of a direct class-field source (`rt share borrowed_box.count`). Scalar field Copy capability is **not** permission to elevate the borrowed class owner's storage into a shared ownership capability.

The checker now uses the shared `share_place_has_owner_authority` predicate for both named variables and direct class fields. It rejects borrowed and mutable-borrowed roots with `E_SHARE_BORROW` before the ordinary field read. It retains the existing non-Copy class/array checks, other source categories, and native source lowering.

**Additional evidence inventory**
- `tests/native_share_place_authority_test.cpp`: ten distinguishable direct owner/field, immutable/exclusive loan, nested projection, array access and computed-place authority classifications. Registered as `native_ownership_share_place_authority` CTest. This is a C++ classifier test **not** a guest compiler test.
- `tests/fixtures/atto-share-borrowed-class-field-reject.atto`: parser-positive *candidate* source for `rt share box.count` when `box` is a `borrow Box` parameter. Registered as `atto_share_borrowed_class_field_rejected` requiring `E_SHARE_BORROW`. A syntax error or feature-unsupported rejection **does not** establish semantic ownership denial.
- Exact-head `atto-critical-ownership-llvm-parity.yml` now runs two focused C++ classifier CTests and expects twenty scoped compiler/guest CTests with no skipped, missing or failed cases.

**Not claimed:** a complete typed MIR, non-lexical lifetime solution, general recursive Share trait, actor Send authority, class-layout nonalias proof, or executed LLVM/Clang guest result. Source-org Actions remains on an admission hold (`steps=null`), independent review required.
