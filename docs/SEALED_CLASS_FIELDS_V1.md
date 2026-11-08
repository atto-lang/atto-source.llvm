# Sealed class-field AOT v1 — strictly scoped native LLVM admission

This is an **experimental opt-in profile** based on the draft
`sealed-scalar-v1` compiler gate. It is a step toward AOT classes, not a
claim of working virtual dispatch, actor heaps, general guest zero-GC,
cross-backend parity, or hot upgrade.

`--aot-profile=sealed-class-fields-v1` admits fully resolved native source
with a finite (max 32) set of immutable, static class declarations, each
with 1–16 literal scalar fields. Instances must be fresh stack-only local
`new Class()` values. Typed scalar field reads are permitted. Class
methods, parameterized constructors, borrows, moves, escaping class ABI
values, mutable fields, nested classes, owned arrays, runtime-generated
definitions and non-scalar effects are rejected before LLVM IR output.
Known direct-function name/arity closure remains required across **all**
functions, including non-exported declarations.

The AOT linker still uses Clang and the existing LLVM IR GC-symbol auditor.
The latter cannot prove that no tracing collector exists in a linked native
dependency. The 15 new fixture cases exercise compiled native output (eight
positives) or compiler-negative no-IR/no-artifact behavior (seven negatives)
**only after** they execute on the exact PR head. A GitHub Actions workflow
with `steps=null` means **NOT_EXECUTED**.

Run on a funded trusted source runner:

```bash
cmake -S . -B build
cmake --build build -j4
ctest --test-dir build -R native_sealed_ --output-on-failure
ORESLANG_LLVM_AOT_PROFILE=sealed-class-fields-v1 scripts/compile-aot.sh app.atto ./app
```

Next: separate verified methods/receiver dispatch graph closure, then actor
heap and continuation ownership with cancellation/restart reclamation.
