# AttoLang native ownership CFG join — complete metadata contract

Status: **DRAFT / native compiler-backed guest and exact-head CI NOT_EXECUTED**.
This child sits on LLVM ownership PR #1455 (which sits on #1395). It does
not implement general path-sensitive MIR loans, borrow lifetimes, native
shared-mutable arenas or actor/tenant transfer admission.

## Finding

The former `same_affine_state` compared the variable inventory and only
four dynamic properties: moved owner, moved fields, shared loan count and
exclusive loan. It omitted static place/capability identity (including
allocation domain, class field classification, mutability, direct assignability,
parameter mode and field visibility). A future or current branch-local change
to one of those omitted properties could pass the conditional/short-circuit
ownership join despite differing successor authority.

## Implementation

`src/main/cpp/oreslang/types/NativeOwnershipJoin.hpp` now owns the one
`LocalOwnership` layout used by the real native `OwnershipChecker.cpp`.
A CFG join must match **all 17** state dimensions for every lexically named
root. Incompatible predecessor state always fails closed; there is no
implicit capability merge and no runtime borrow table.

1. parameter mode
2. root moved flag
3. heap ownership
4. array ownership
5. record view
6. mutable-owner capability
7. slot assignability
8. shared loan count
9. exclusive loan flag
10. class nominal identity
11. mutable field paths
12. nested subobject identity
13. class field inventory
14. class array field inventory
15. final field set
16. public field set
17. moved field paths

`tests/native_ownership_join_test.cpp` uses the **production header** and
mutates each dimension independently, checking symmetric rejection, identical
state acceptance and inventory mismatch. CMake registers this as
`native_ownership_affine_join_complete`; the source-owned exact-head workflow
builds and runs that target in addition to the earlier 17 compiler CTests.

## Evidence and limits

- During authoring, an isolated local C++17 test using the matching comparison
  implementation and a minimal parser-mode stand-in passed all 17 field
  mutation checks. Deleting the heap-owned comparison made the isolated test
  fail as intended. **That was a host-only algorithm check, not an exact
  compiler-head production build or AttoLang guest run.**
- The source-owned GitHub Actions workflow must execute nonzero steps at the
  exact immutable PR head, compile the actual production header plus compiler,
  then upload compiler/test receipts. A run with `steps=[]` or `steps=null`
  is runner admission failure, not a C++ or native-guest failure/pass.
- Full CFG soundness needs path-sensitive source places, effect summaries,
  scoped and nonlexical loan proofs, and lowering invariants. This patch only
  ensures that a join cannot discard a difference in currently tracked
  metadata.

Review and security: independent review of the exact tree is required; do not
merge zero-step evidence; do not copy private source across organizations.
