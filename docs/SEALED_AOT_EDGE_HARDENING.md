# Sealed AOT edge hardening

Stacked on the immutable class-field admission PR. The verifier rejects unmodeled statement variants, scoped class-owner shadowing, mutable class owners, and non-leaf literal class defaults before LLVM IR.

The new `native_sealed_aot_profile_mutations` CTest includes 12 ordinary-compiler-versus-sealed contrasts. The ordinary LLVM compiler must first emit function IR; the sealed compiler must then reject with a specific diagnostic, no IR, and no artifact. Three cases are source mutations of a valid class example and one adds an unreachable IO helper. These are source mutation tests, not compiler-implementation mutation testing.

Tests are **NOT_EXECUTED** without exact-head nonzero-step CI; `steps=null` is not evidence.
