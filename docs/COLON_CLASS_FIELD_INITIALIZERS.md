# Native class field initializer parity (bounded scalar slice)

Canonical source fields use `:` and the `=` spelling is admitted as compatibility syntax:

```ores
define class Foo as
  final int foo: 5
  final const int number: 9 // accepted alias; formatter emits 'final int number: 9'
  int bar: 37
end
```

The native LLVM frontend admits literal-only `final const int` as a compatibility
alias of `final int`; the extra `const` does not change the native field slot.
Arbitrary expressions remain unsupported and must fail before IR is emitted.

The native LLVM frontend accepts these type-first i64 literals (with or without
a semicolon when a real newline separates fields). Old `let int foo = 5;`
and `let mut Array<int> items = new Array<int>();` syntax remains admitted.

`final` is a compiler-owned fixed-field capability, not a reflection flag.
The native ownership checker prevents ordinary writes, destructive `rt take`,
conditional writes, and deferred/finally writes to final fields, even on
an otherwise mutable class owner. The class ABI/layout is unchanged.

**Scope:** The native source subset still cannot lower `string` class
fields, arbitrary expressions as field initializers, nominal `struct`
declarations, or dynamic field schemas. Unsupported syntax must fail before
IR emission, rather than emulating string values in i64 storage.

Verification gate: `native_colon_class_field_admission` and optional
`native_colon_class_field_execution` CTests compile the source, reject final
writes/takes, check no IR on failure, and execute compiled LLVM via Clang.
They are not considered passed until a real test run completes.

For canonical formatting and full Java field syntax, coordinate with the
`oreslang-format` and `oreslang-source.java` branches.
