# Java ↔ LLVM local aggregates — explicit stage-1 parity gate

**Status (27 October 2026):** LLVM parser/AST recognition plus a gated,
unverified compiler lowering slice for scalar callable-local nominal structs.
**There is no successful exact-head compiler/OS execution evidence yet.** Java
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
  layout identity derived from its callable name, source position and local
  type name (not an exported or cross-module-stable symbol).
* A public ordinary `main(): int` (or other scalar-return function) may
  construct that type through `val T p = T{...}`, read or write its
  scalar fields using the existing native checked field-write path, and
  return scalar results. The owner remains a native stack class value, not
  a pseudo-i64 struct. `const T` is not admitted in this first slice.
  Mutation remains subject to existing ownership and borrow checks.
* An unexported private function may return the same locally named nominal
  type through the existing checked immutable-leaf record region ABI.
  Its fields are **final** in this return-handle slice and cannot be
  reassigned; exported hidden returns remain forbidden.
* Named arguments are checked against the declaration and reordered into
  native layout order. **Named initializer values must be integer literals**
  until source-order-preserving ANF staging exists: reordering side-effecting
  argument expressions would be a semantic bug. Positional constructor
  expressions preserve source order. Missing/extra/duplicate fields, non-int
  field definitions and mutable/fallible representations remain gated.

Executable native test fixtures now include `point.x = 10` and a
field-to-field arithmetic update. Private-return mutation and `const`
owners have compile-negative fixtures. These additions are **unverified**
until a native runner executes their exact source tree.

This does **not** prove that the generated IR compiles and runs; only a
successful exact-head compiler run followed by Clang/OS execution can establish
that evidence. The CTest matrix requires native Clang/OS execution of its
scalar field-read cases and **fails if Clang is missing**; it never reports a
passing result for IR-only compilation.

Java PR #2179 remains syntax-stage only. These LLVM-specific compiler
narrow-slice changes do **not** imply Java/LLVM feature or semantic parity.

## Native sanitizer gate (still unverified)

CTest includes **three distinct kinds of checks**, each with a separate
evidence boundary:

* `native_local_aggregate_types_stage1` links and runs ordinary LLVM IR
  executable fixtures and checks fail-closed compile diagnostics.
* `native_local_aggregate_asan_harness_contract` is a **source-free**
  Clang/LLVM IR canary: it proves `-fsanitize=address` alone fails the
  memory-access instrumentation assertion on pre-generated IR, then adds
  the LLVM `sanitize_address` function attribute, confirms actual ASan
  access checks, and links/executes a benign probe.
  `native_local_aggregate_types_asan_guest` applies this **verified ASan
  instrumentation** to admitted Oreslang-generated LLVM IR, requiring
  correct process exits, leak detection and fail-fast memory checks.
* `native_local_aggregate_types_asan_ubsan_compiler` is registered **only
  when** configuring a separate build with
  `-DORES_LOCAL_AGGREGATE_COMPILER_SANITIZERS=ON`. In that configuration
  `oreslang-llvmc` itself is compiled and linked with
  `-fsanitize=address,undefined` (and nonrecovering diagnostics), so the
  C++ compiler's own code is checked while compiling all local-aggregate
  fixtures. The emitted guest IR gets verified ASan instrumentation.

**Limitation:** `-fsanitize=undefined` at the IR linking stage cannot
retroactively generate Clang frontend UBSan checks for an already-emitted
guest LLVM module. Compiler UBSan coverage is therefore reported
separately from guest ASan coverage; full guest-language arithmetic/
undefined-behavior checking requires explicit codegen guards.

When an approved exact-head runner is available, execute both build modes:

```bash
cmake -S . -B build/native -DCMAKE_BUILD_TYPE=Debug
cmake --build build/native --target oreslang-llvmc -j2
ctest --test-dir build/native -R 'native_local_aggregate' --output-on-failure

cmake -S . -B build/compiler-sanitized -DCMAKE_BUILD_TYPE=Debug \\
  -DORES_LOCAL_AGGREGATE_COMPILER_SANITIZERS=ON
cmake --build build/compiler-sanitized --target oreslang-llvmc -j2
ctest --test-dir build/compiler-sanitized -R 'native_local_aggregate' --output-on-failure
```

Missing Clang, absent sanitizer runtime, missing ASan access checks, wrong
native exit code, sanitizer diagnostics, or a negative case emitting IR
must fail the relevant gate. **No compiler, native executable or CTest pass
has yet been obtained at the exact PR head.**

These checks are *registered, not passed*. Source Actions jobs still fail
before executing steps. A source-free private carrier was verified with 13
completed steps on
[`elenkos-systems-test/clients-rust-consumer` run 40583274205](https://github.com/elenkos-systems-test/clients-rust-consumer/actions/runs/40583274205),
but that carrier's `main` and source `main` are unprotected, and the
LLVM source allowlist is empty. Do not mirror private LLVM source until
independent reviews and enforceable branch protections are complete; see
LLVM issues #915, #475 and #159.

## Follow-up gates (in order)

1. **Java + LLVM:** front-end lexical symbol table with predeclaration of
   direct callable-root types for forward return signatures. Reject nested
   type escapes, alias cycles, duplicate/public-private collisions and
   recursive unboxed layout. Preserve nominal struct vs structural record.
2. **Typed construction:** exact arity/type checks, named and positional field
   correspondence, duplicate and missing fields, definite initialization,
   read-only record capabilities, owner/borrow mutation distinction, and
   correct source-order evaluation for nonliteral named arguments.
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
`native_local_aggregate_types_stage1` in CTest. It checks explicit rejection,
strict stack aggregate compilation, separate callable nominal identities and
private record-return LLVM IR. **An actual Clang link+execute pass** is required
to claim execution evidence; IR-only success is insufficient. The source
organization's previously observed zero-step Actions failures do not constitute
compiler or sanitizer results. A check of six proposed private alternate
repositories found recent zero-step job failures. A separate private
source-free carrier (`elenkos-systems-test/clients-rust-consumer`) has
executed verifier steps, but its source-bearing test jobs were skipped and
its `main` remains unprotected. The source repo's
`.github/private-ci-approved-repositories.txt` currently approves **no
cross-organization destinations**. No private source may be mirrored without
independent maintainer/security approvals and a reviewed protected-branch
allowlist change. Keep the PR draft until exact-head execution and a sanitizer
audit succeed. See source CI issue #159 and private transfer review issue #475.
