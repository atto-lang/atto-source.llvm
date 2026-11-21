# AttoLang compiler-owned core/alloc import parity — LLVM

This milestone makes new AttoLang source code use `.atto` imports without
duplicating compiler-owned modules or weakening freestanding provider checks.
The LLVM binary/repository names remain Oreslang-named for compatibility.

## Source-level imports

| Canonical AttoLang import | Compatible legacy spellings | Unique compiler-owned unit |
| --- | --- | --- |
| `atto.core/numeric.atto` | `atto.core/numeric`, `atto.core/numeric.ores`, `ores.core/numeric`, `ores.core/numeric.atto`, `ores.core/numeric.ores` | `@atto.core/numeric.atto` |
| `alloc/int_buffer.atto` | `alloc/int_buffer`, `alloc/int_buffer.ores` | `@alloc/int_buffer.atto` |
| `alloc/int_buffer_bundle.atto` | `alloc/int_buffer_bundle`, `alloc/int_buffer_bundle.ores` | `@alloc/int_buffer_bundle.atto` |

All aliases return the **same registry source entry**; module linkage deduplicates
by the canonical internal unit ID. An imported module must not acquire a fresh
instance, change its provider closure or gain permissions by selecting a legacy
or modern spelling.

The `atto.core/*` namespace is compiler-reserved. A project-local
`atto.core/numeric.atto` file is not a substitute for the compiler-owned
implementation, even when the file declares the same module name. The legacy
`ores.core/*` namespace remains reserved. Unknown, traversal, and foreign
suffix forms are rejected without emitting LLVM IR.

## ABI and target boundaries

- The `numeric` core module is pure, allocator-free and supported as a
  source-level compiler-owned module on LLVM's admitted native and freestanding
  ARM/Wasm target triples.
- The `alloc/int_buffer` module requires the native allocator provider.
  Its `int_buffer_bundle` dependency closure carries that same requirement
  even when the bundle's visible function does not allocate.
- Freestanding ARM/Wasm targets must **reject** allocator-dependent core
  modules when no approved allocator provider is supplied. A suffix or
  namespace alias must never bypass that rejection.
- Host LLVM IR is linked with Clang and executed only in the native-host
  test path; ARM/Wasm IR/triple checks are **not** equivalent to executing
  an embedded device or WebAssembly guest.

## Required tests and receipts

The existing CTest gates now cover:

- `native_core_library_registry_provider_policy`: actual C++ registry
  identity, reserved namespace and provider-closure assertions
- `native_core_library_import_resolution`: real `.atto` compiler
  invocation; LLVM IR identity/negative diagnostics; host Clang linking,
  executable exit code 42; ARM and Wasm core source admission
- `native_alloc_library_import_resolution`: new `.atto` direct and
  transitive imports, single definition and allocator ABI checks; Clang
  native execution; fail-before-IR ARM/Wasm tests without allocator provider

The exact-head import workflow runs these CTests together with the existing
canonical source/project-root tests. It requires nonempty native Clang
configuration rather than silently skipping linking.

**Evidence boundary:** Code and test specifications are not execution
receipts. Source-organization GitHub Actions has repeatedly returned
`steps=null` due to runner admission failure; those runs are
`NOT_EXECUTED`. Do not claim LLVM guest parity until the exact immutable
head/tree has real compiler, link, executable, sanitizer and negative-case
receipts. Source-free canaries in other organizations are not substitutes.
