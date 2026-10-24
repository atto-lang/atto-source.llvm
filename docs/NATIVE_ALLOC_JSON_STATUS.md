# Native alloc/json substrate status

Status: **alloc/json substrate implemented; guest-source lowering intentionally unavailable**

The native LLVM backend now carries an OS-independent but allocation-dependent JSON text/tree substrate in
`src/main/cpp/oreslang/runtime/AllocJson.hpp`. The filename/class names deliberately identify allocator-backed machinery for the canonical `alloc/json` contract in `oreslang-common-source#111`; they do not establish an allocator-free `core` API.

## What is implemented

`AllocJson` currently owns JSON text and dynamic tree mechanics without an OS API or a
third-party JSON dependency. Because the implementation owns growable strings/arrays/objects, it requires allocation and must remain outside the allocator-free `core` closure. It provides:

- strict JSON parsing and deterministic encoding;
- insertion-ordered object values;
- exact JSON number lexemes rather than host floating-point coercion;
- duplicate-key rejection;
- UTF-8 and `\uXXXX` surrogate validation;
- signed-i64 exact extraction with overflow rejection;
- bounded input/output, nesting, node count, container size and number length;
- encode-time duplicate-key and UTF-8 validation for programmatically built
  values.

The implementation is intentionally a value tree, not a host-reflection API.

## What is not implemented

The current LLVM source frontend is a constrained typed-i64/class/array subset.
It does not yet have general source string literals, `Map<K,V>`, structural
record/struct values, or shape-changing expressions. Therefore this branch does
**not** claim source-level support for:

- `Json.parse(...)`, `Json.encode(...)`, or `Json.decode<T>(...)`;
- `Map<K,V>` or `m[key] = value`;
- structural `value with { field: value }` updates;
- typed JSON construction of records, structs or classes.

Those forms must continue to fail closed in the native frontend until their
value representation, ownership/drop rules and LLVM lowering exist.

## Required source-level parity gates

Before exposing the Java/Graal surface syntax in the LLVM compiler, native work
must provide all of the following together:

1. owned UTF-8 string values and literals with deterministic drop semantics;
2. an owned map representation with typed key/value ownership and checked
   insertion/replacement;
3. closed record/struct layouts and exact shape typing;
4. ownership-safe shape updates that cannot shallow-copy move-only children;
5. compiler-issued JSON target descriptors rather than host reflection;
6. typed decode construction rules for nominal classes that cannot bypass
   constructors, initializers or actor isolation;
7. source-to-LLVM tests for malformed JSON, duplicate/unknown/missing keys,
   numeric overflow, Unicode, ownership moves and exact structural shapes;
8. exact-head native build/CTest and real generated-code execution evidence.

Until those gates exist, the portable allocator-backed codec is useful runtime machinery
but is **not evidence of Java/LLVM source-language parity**.

## Verification scope

`tests/native_alloc_json_test.cpp` exercises the native substrate directly,
including large exact-number round-trip, duplicate-key rejection, malformed
number rejection, Unicode surrogate handling, invalid UTF-8 rejection,
signed-i64 overflow, number-length bounds, encode-time duplicate-key rejection,
invalid encoded UTF-8 and depth limits.

This test is registered in CTest with strict warnings. A complete source-level
JSON conformance suite belongs only after the missing native value/lowering
layers above are implemented.


## Placement gate

The backend-facing naming is intentionally `AllocJson`: no public or guest-visible API may imply that an owned dynamic JSON tree is available in allocator-free `core`.

A separate future `core` JSON surface may support borrowed tokenization/parsing and caller-provided fixed buffers/sinks with zero allocation; that surface must not return this owned tree type.
