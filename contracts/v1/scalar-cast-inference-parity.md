# Scalar inference and checked-cast parity (v1)

Status: proposed compiler contract; tests are authored, **not** evidence of executed exact-head CI.

## Meaning of the two spellings

```ores
const int x = 7;
const y = 7 as int;
```

Both bindings have static type `int`, immutable slots and value 7.

* `const int x = e;` checks that `e` is assignable to the declared `int`. This is **not** a cast.
* `const y = e as int;` checks the `as int` expression and infers `y` from its result. `as` is an **explicit checked cast**, not an unchecked reinterpretation and not proof that the inference engine picked the same type without the cast.
* `const z = e;` infers from `e` alone. Tests must independently verify that this infers `int`; testing only `e as int` is insufficient.
* `as?` returns `Option<T>` on backends with checked optional-cast lowering (Java). It must never silently turn into `T`, discard `None`, or become a native zero.
* Immutable-by-default bindings never become reassignable through an expression cast. An explicit `let` controls rebinding independently.

No implicit numeric conversion, string parsing, bool-as-number coercion, lossy narrowing, changing ownership, or bit reinterpretation is permitted by a declaration or a cast. Invalid unrelated scalar casts are compile-time errors. A cast that can succeed only at runtime needs a reifiable runtime check or is rejected; there is no unchecked fallback.

## Executable parity slice

| Feature | Java/Graal | LLVM/native v1 |
| --- | --- | --- |
| `const int x = 7` | type check | type check |
| `const x = 7` | infer int | infer int |
| `const x = 7 as int` | checked identity | checked identity |
| `const x = true as bool` | checked identity | checked identity |
| `const x = "s" as string` | checked identity | checked identity |
| `const x = foo()` | inferred call result | inferred scalar call result |
| `const x = condition ? a : b` | common type | same-type scalar arms |
| scope-local `const x = e` | inferred | inferred scalar |
| `const x = 7 as bool` | reject | reject |
| `as?` or nominal/dynamic casts | checked/runtime where reifiable | **reject until checked lowering exists** |
| arbitrary ADT, tuple, record, class, generic inference | Java typechecker, feature-dependent | **not proven: no claimed parity** |

The native backend's first step is *scalar identity casting only* (including resolved scalar aliases). It must reject any nominal/dynamic target, runtime-unreifiable target, or optional cast until a checked representation and ownership-proof path exist.

## Minimum execution requirements

1. Java: execute the `ScalarCastInferenceParityTest` JUnit test class, including guest execution, at the PR head and record its executed steps.
2. LLVM: compile, link and run the *native Oreslang programs* added to the existing `native_source_bool_${mode}` CTest jobs, plus negative compiler fixtures with empty LLVM IR on failure. Test native Release and sanitizer variants.
3. Confirm the exact head Git tree, compiler commit, source fixtures, toolchain revisions and output/exit receipts on each backend. Different commits are not the same execution evidence.
4. Separately cover overflow limits, Option/Result/ADT casting, dynamic type tests, generic type aliases and ownership/borrows when corresponding LLVM lowering exists.
5. Keep source-only assertions, zero-step runner admission failures, successful host tests, executed compiler-backed guests, and independent review distinct. No merge without stepful exact-head green checks and policy approval.
