# AttoNATS String.split_literal — native LLVM owned Array<String> tranche

## Canonical Atto source and scope

[AttoNATS source](https://github.com/atto-poc/atto-nats-clone.atto/pull/27)
`src/core/subjects.atto` is pinned byte-for-byte in this compiler
at Git blob `538a98c2ec79a4d859cd41ac6b85fde9f62e6869`.

This branch adds `String.split_literal(String)` for **lexical synchronous**
`val parts = subject.split_literal(".");`. It produces a uniquely owned
`Array<String>` with an opaque, generation-checked native array handle.
A hidden compiler-owned output region, handle-failure shadow and deterministic
drop epilogue are mandatory. This is not a host-language NATS broker, and
does not enable returning an array from functions, cross-actor array sharing,
network transport, or async source suspension.

Core operations:

- String source and delimiter must both be statically typed `String`.
- One positional delimiter argument; wrong arity/type/named arguments fail
  before any LLVM IR is emitted.
- The delimiter is **literal bytes**, not regex; embedded NUL and UTF-8
  are preserved as byte sequences. Consecutive/leading/trailing delimiters
  emit empty tokens; source `""` produces one empty token.
- Empty delimiter is deliberately fail-closed at runtime until exact
  Java/GraalVM empty-delimiter semantics and a bounded contract are proven.
- The immutable String region retains the original byte storage until the
  invocation ends. Split tokens are non-owning view String handles, and
  the `Array<String>` owns the handle vector (not raw byte pointers).
- The splitter validates a 65,536-token bound, the existing String-region
  65,536-handle/64-MiB budgets, checked native array region capacities, and
  a 128-MiB conservative worst-case comparison-work budget
  (`source_bytes * separator_bytes` tested without integer overflow).
  Search uses `memchr` for the delimiter's first byte; a one-byte NATS dot
  never invokes per-candidate multi-byte `memcmp`.
  Scratch allocations, array reserve and String metadata publication are
  staged: nonzero resource statuses cannot leave partially published guest
  array elements. Existing array/resource failure epilogues drop the owner.
- This kernel is **not guaranteed allocation-free**, zero-copy across actor
  domains, or hard real-time. It avoids **per-token byte copying** within a
  single invocation but allocates array backing and metadata/scratch memory.
  The existing no-GC IR screen does not certify a whole linked image.

Parser `elif` is also lowered to nested native checked conditionals,
sharing the single outer `fi`. This is necessary for the exact canonical
`valid_subscribe_pattern()` implementation in AttoNATS; no source rewrite
of `elif` is accepted as parity proof.

## Required real execution

1. Native C ABI CTest `native_string_split_owned_array_region` compiles
   `NativeStringRegion.c`, `NativeStringSplit.c`, and
   `NativeIntArrayRegion.c` and checks exact token counts/content,
   embedded NUL, empty tokens, multi-byte separators, over-budget rejection
   with an unchanged destination, and deterministic cleanup.
2. LLVM source conformance harness
   `tests/native_class_string_parity.rs` compiles a positive
   `val parts=subject.split_literal(".");` guest and rejects five
   malformed-source/owner escapes without partial LLVM IR.
   It links the real split runtime rather than a Rust implementation.
3. Exact-source AttoNATS:
   `native_atto_nats_canonical_subjects_aot` checks the source Git blob,
   strips only the first-line `use namespace` headers in a temporary
   concatenated compilation unit, compiles the remaining Atto bodies
   **unchanged**, requires real String substring/split and array-get LLVM
   calls, screens no-GC IR, then runs real Clang O0/O2 native binaries.
   The guest exercises subject validation, wildcard syntax/matching and
   malformed subject rejection. Three no-IR negative programs also run.

Run when an **authorized, exact-head local LLVM checkout** is available:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --parallel 2
ctest --test-dir build -R 'native_string_split_owned_array_region|native_class_string_parity_(release|sanitizer)|native_atto_nats_canonical_subjects_aot' --output-on-failure
```

The current source organization's Actions have repeatedly failed at
runner admission with zero executed steps. This code is **candidate work,
not an executed proof**, until the exact source head actually compiles,
runs and passes CTest and Clang guest execution with sanitizer receipts.
Do not export private LLVM compiler source to an unapproved carrier;
the protected-main source policy is binding.

## Explicit remaining gaps

- Verbatim multi-file `use namespace` lowering/import resolution;
  the harness removes only those two known first-line headers.
- Per-actor sealed `state{}`, generation-fenced critical read/write
  loans, authenticated subscription indexes and ownership across await.
- Nonblocking native Bytes/TCP NATS CONNECT/SUB/PUB/MSG delivery, queues,
  slow-consumer flow control and forced-disconnect lifecycle.
- Real end-to-end throughput, latency, CPU, allocation/GC and independent
  Java/GraalVM plus upstream NATS comparative benchmarks.

**Keep DRAFT until executable compiler/sanitizer proof and independent
security/ownership review complete.**
