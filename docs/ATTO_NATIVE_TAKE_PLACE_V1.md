# AttoLang native `rt take` source-place admission

Status: **DRAFT — compiler-backed guest execution NOT_EXECUTED**. Child of
LLVM ownership CFG join #1482 (itself stacked on #1455 and #1395).

## Exact issue

Before this patch, the native checker recognized named-variable and direct
named-class-field transfers and invalidated their source places, but other
expressions could reach `case Expression::Kind::Take`, pass `check_value`,
and return without invalidating any source. Conditional selection was already
rejected by #1455. Java Wave 1 independently has
`rejectUntrackedTakePlace`, so backend admission semantics differed.

## Bounded implementation

`NativeOwnershipPlace.hpp` is the compiler-shared, total source-place
classifier. Stage-one LLVM only admits:
- `rt take owner`, where `owner` is a tracked named binding
- `rt take owner.field`, where `field` is a direct named class field
  (existing mutable/final/borrowed/move checks still apply)

It denies computed/untracked rvalues, function results, nested projections,
borrow wrappers, unary/binary operations and unnamed AST nodes with
`E_TAKE_UNTRACKED_PLACE`. Conditional `rt take` retains the older
`E_TAKE_UNTRACKED_SELECT` diagnostic. The check happens **before**
evaluation; source place validation for named owners/fields then proceeds
through the existing compiler checker and CFG logic.

This is a closed-world native stage-one limitation, **not** a claim that a
general `rt take` of rvalues is always unsound. General rvalue ownership and
nested partial moves require typed MIR place/region semantics and verified
lowering before future admission.

## Evidence and CI gate

- The exact authored classification header and test logic were compiled and
  executed in an **isolated host C++17 harness** using a minimal Parser
  `Expression` enum stand-in. GCC and Clang both reported the expected
  classifications for the two allowed source categories and 21 malformed or
  computed alternatives.
- Deliberately permitting `Add` in the classifier caused the test to fail
  with exit 7. This is a real local **host-only mutant**, not native Atto
  frontend execution or exact GitHub head execution.
- Production CMake now builds the classifier test from the same header as
  `OwnershipChecker.cpp`, registers
  `native_ownership_take_place_classification`, and a real compiler fixture
  `atto-take-computed-untracked.atto` expecting
  `E_TAKE_UNTRACKED_PLACE`.
- The exact-head LLVM CI job is expected to execute the two native host CTests
  and 18 compiler/guest CTests and verify the complete count before review.
  `steps=[]` or `steps=null` means runner admission failure,
  **NOT_EXECUTED**, irrespective of a workflow failure label.

No source was copied into an unrelated GitHub organization. No runtime
borrow tracking, general nonlexical loan MIR, actor-crossing reference
authority or five-backend parity is claimed. Keep this draft until
stepful exact-head compiler tests and independent review have succeeded.
