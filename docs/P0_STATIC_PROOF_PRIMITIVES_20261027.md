# P0 static proof helpers — LLVM/C++17

**Status: compiler-core helper and host-only CTest work. No Oreslang guest parser/LLVM IR lowering or native executable proof is claimed.** Language program: [shared feature contract #172](https://github.com/ores-truffle-oreslang/oreslang-common-source/pull/172), [LLVM issue #1084](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/issues/1084).

## New components

- `NativeAssociatedWitnessContract.hpp`: finite static protocol-head and fully grounded associated equations, unique dictionary symbols, orphan checks, generic unification/coherence, ground parameter substitution, import-order-invariant witness selection and node budgets. No runtime witness registry.
- `NativeSessionProofContract.hpp`: finite one-session CFG phase analysis with all-predecessor union at joins; wrong-phase, copied handle, generation rebound and post-admission timeout reuse reject. Does not mint real runtime capabilities or prove physical generation fencing.
- `NativeRefinementProofContract.hpp`: i64 interval implication for function call preconditions/normal-return postconditions, and inferred-effect admission with UNKNOWN denial. This branch now implements portable, **overflow-checked i64 interval addition, subtraction and multiplication** (no signed-overflow UB or nonstandard `__int128` dependency), including signed extrema. This is a conservative interval checker, **not** general SMT completeness, compiled guest IR lowering or hardware WCET proof.

The new test executables are registered in `CMakeLists.txt`, and standalone C++17 GCC/Clang test workflow is at `.github/workflows/p0-native-static-proofs.yml`. Locally runnable independent of LLVM libraries:

```sh
c++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror -UNDEBUG -I src/main/cpp tests/native_associated_witness_contract_test.cpp -o /tmp/p0-witness && /tmp/p0-witness
c++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror -UNDEBUG -I src/main/cpp tests/native_p0_session_refinement_test.cpp -o /tmp/p0-session && /tmp/p0-session
```

## Required downstream gates

1. Native Parser/OwnershipChecker/typed CFG actually invokes these proofs; reject unsupported syntax with no emitted IR.
2. Source-negative cases: illegal phase, moved session, copied actor token, ambiguous generic associated witness, orphan import, invalid interval postcondition/unsafe narrowing, unknown effect.
3. Real LLVM IR generation from `.ores` and linked native executable executes correct actor protocol and checked result; no JIT/AST fallback.
4. Differential observed traces against Java Native Image *program-specific guest* implementations; source/IR/binary digests, sanitizers, loan/CONTROL/cancellation/generation gates.
5. AOT != hard realtime. Release only with bounded platform-specific WCET/schedulability proof.

Source-org CI infrastructure blockers and host-only C++ tests do not constitute guest parity.


## Subsequent hardening (2026-10-27)

- Verified the portable i64 arithmetic kernel locally against Boost.Multiprecision arbitrary-precision integers: **150,225 operand pairs, 450,675 operation comparisons**, GCC and Clang with ASan/UBSan. These tests check the extracted arithmetic algorithm, not an exact-tree source compiler build.
- Reject C++ enum-cast effect values outside the declared vocabulary, and reject blank or >256-byte resolved callable symbols. Additional negatives live in `tests/native_p0_session_refinement_test.cpp`.
- Carrier candidate [opto-sync-test/go-java-validation PR #30](https://github.com/opto-sync-test/go-java-validation/pull/30) has a proposed immutable LLVM 16-command GCC/Clang O0/O2 ASan+UBSan test plan, but **no source branch has been mirrored or executed** on a verified available GitHub runner. Preserve fail-closed `not_executed` status.
