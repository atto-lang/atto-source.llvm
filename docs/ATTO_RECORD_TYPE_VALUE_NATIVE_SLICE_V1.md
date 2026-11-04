# AttoLang record type/value context — native LLVM v1 slice

This describes a **candidate** implementation on LLVM PR #1114, not proven
end-to-end backend parity. Java/GraalVM PR #2177 defines the broader
record type/value grammar contract. Both require separately executed
exact-source compiler tests.

## Source-level distinction

A lexical `type T = record { foo: int };` defines a compile-time closed
type. `record { foo: 42 }` is an executable value, not a declaration.
This bounded native subset can initialize a `const` typed local using a
matching inline record/inferred record/inferred struct literal:

```atto
pub fnc main(): int {
    type Row = record { foo: int };
    const Row row = record { foo: 42 };
    return row.foo;
}
```

```atto
pub fnc main(): int {
    type Point = struct { x: int };
    const Point point = infer struct { x: 43 };
    return point.x;
}
```

The parser evaluates each source field once in lexical order, retains a
closed static aggregate kind/shape, and requires exact shape/kind matching
for typed immutable locals. Records and structs **must not** be erased to
the same type merely because their scalar fields have identical layout.
The implementation delegates to the existing stack-only aggregate
lowering and ownership gate, not a dynamic map or reflection.

## Explicit boundaries

- Only synchronous stack-local closed `int` and `bool` fields are
  currently eligible; this change does not add `float`, `string`,
  nested aggregates, functions, methods, heap storage or transport.
- `val`/`let` mutable aggregate aliases, dynamic computed keys,
  aggregate call ABI, aggregate return ABI, tuple indexing and
  uninitialized partial shapes remain outside this slice.
- Generic `infer<T>` name capture, a `const type T value = ...`
  marker, cross-module aliases and Java's name-first binding spelling
  have **not** been shown to work in native LLVM; do not treat those as
  parity.
- `const type T = record { ... };` is not a type declaration. It must
  reject before LLVM IR and must not silently materialize a value.
- Type, field-name and field-type mismatches must fail before IR, with
  no partial generated artifact. Structural parameter/pattern/actor
  transport remains explicitly unsupported.

## Acceptance and negative proof

The following **actual compiler/guest** gates must execute against one
exact head/tree before this slice may be called verified:

```sh
cmake -S . -B build
cmake --build build --target oreslang-llvmc oreslang-native-structural-gate-build
ctest --test-dir build --output-on-failure \
  -R 'native_aggregate_exact_struct_guest_execution|native_structural_shape_failclosed'
```

`tests/native_aggregate_positive_execution.py` includes seven
source-to-LLVM-IR-to-Clang-to-OS guest cases with exact exit statuses
and seven no-IR negative controls. These now include typed record
values, typed inferred record and typed inferred struct values, plus
kind/name/type mismatch rejections. Its independent source-free test
`tests/test_native_aggregate_positive_execution_harness.py` checks
the acceptance harness only and must not be promoted to guest evidence.
`tests/native_structural_shape_failclosed.rs` adds the same
type/value-context admission controls with no-IR negative checks.

Require: exact source commit/tree, nonzero GitHub job steps, compiler
and Clang identity, LLVM IR integrity, actual OS exit, sanitizer
execution where supported, and negative diagnostics. Failed GitHub
Actions jobs with `steps=null` are **NOT_EXECUTED**. No private code
can be mirrored into another org without independently approved
protected-source carrier authorization.
