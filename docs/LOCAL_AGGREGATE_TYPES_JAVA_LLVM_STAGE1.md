# Java ↔ LLVM local aggregates — explicit stage-1 parity gate

**Status (27 October 2026):** LLVM parser/AST recognition and rejection gate only.
This is a compatibility foundation, **not** executable source parity. Java
[PR #2179](https://github.com/ores-truffle-oreslang/oreslang-source.java/pull/2179)
likewise stages the parser/AST without approving execution.

## Accepted syntax for parser-only testing

```oreslang
pub fnc foo(): T {
    struct T { int foo }
    return T{5};
}
pub routine bar(): U {
    type U = struct { foo: int }
    return U{foo: 5};
}
pub fnc baz(): V {
    record V { int foo }
    return V{5};
}
pub routine qux(): W {
    type W = record { int foo }
    return W{5};
}
```

The AST records the declared type name, ordered field names/types, whether
a declaration is a nominal struct or a transparent struct/record alias,
and whether a constructor is a locally declared `T{...}`. The parser
checks duplicate type/field names, mixed constructor argument modes, and a
64-field source budget before typed lowering. No type construction is run at
invocation time.

Unlike Java's broader block AST, LLVM stage 1 only recognizes declarations
at a callable root. Nested block declarations are explicitly rejected until
lexical scope ownership, capture and escape rules are implemented.

## Fail-closed compiler boundary

All local aggregate forms outside the explicitly gated native scalar
subset below fail `LOCAL_AGGREGATE_ABI_UNSUPPORTED` in `Parser::validate`,
before ownership and LLVM lowering. This is intentional. In particular, the locally declared
`T` in the public return signature is not interchangeable with a class
or `i64`. A syntax-only parse is *not* proof of valid code, stack allocation,
zero-copy transport, stable identity, or an exported ABI.

Diagnostics also reject `const type`, local classes, duplicate declarations,
invalid field names, and mixed positional/named `T{...}` arguments. Unsupported
syntax must not emit LLVM IR or silently coerce records into structs.

## Stage-2 native scalar subset (draft, unverified)

The follow-on commits add *implementation paths* for a deliberately small
native subset, reusing already-established class layout and ownership lowering:

* A **nominal** callable-local `struct T` with 1–16 `int` fields, a single
  declaration per function, zero explicit parameters/generics, no declared
  effects and no source suspension. It receives a compiler-owned, unspellable
  layout identity derived from its source position and local type name.
* A public ordinary `main(): int` (or other scalar-return function) may
  construct that type through `val T p = T{...}`, use its fixed fields,
  and return scalar results. This constructs an actual native stack class
  value, not a pseudo-i64 struct.
* An unexported private function may return the same locally named nominal
  type through the existing checked immutable-leaf record region ABI.
  Exporting this hidden return type remains forbidden.
* Named arguments are checked against the declaration and reordered into
  native layout order. Missing/extra/duplicate fields, non-int field
  definitions and mutable/fallible representations remain gated.

This does **not** prove that the generated IR compiles and runs; only a
successful exact-head compiler run followed by Clang/OS execution can establish
that evidence. The CTest matrix requests three runnable field-read cases
when Clang is available; it otherwise reports IR-only coverage.

Java PR #2179 remains syntax-stage only. These LLVM-specific compiler
narrow-slice changes do **not** imply Java/LLVM feature or semantic parity.

## Follow-up gates (in order)

1. **Java + LLVM:** front-end lexical symbol table with predeclaration of
   direct callable-root types for forward return signatures. Reject nested
   type escapes, alias cycles, duplicate/public-private collisions and
   recursive unboxed layout. Preserve nominal struct vs structural record.
2. **Typed construction:** exact arity/type checks, named and positional field
   correspondence, duplicate and missing fields, definite initialization,
   read-only record capabilities, ownership/borrow and mutation distinction.
3. **AOT representation:** aggregate LLVM types and checked stack-vs-arena
   allocation selected by escape analysis (not by source spelling alone);
   real return/call ABI descriptors and stable compiler-generated names.
   Constructor expressions must never resolve through a class constructor
   or the scalar `i64` placeholder path.
4. **Conformance:** exact-source compile/execute fixtures, ASan/UBSan,
   valgrind when available, cross-module public ABI tests, and independent
   Java/Graal, native LLVM, .NET AOT, Wasm and Forge runs at exact head SHAs.
   Separate infrastructure failures/zero-step Actions from executed tests.

Test entrypoint: `tests/native_local_aggregate_parity_stage1.py` registered as
`native_local_aggregate_types_stage1` in CTest. Its success demonstrates
only parser recognition, deterministic rejection and preservation of existing
scalar compilation.
