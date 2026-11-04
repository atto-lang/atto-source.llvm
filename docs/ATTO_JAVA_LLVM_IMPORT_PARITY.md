# Canonical AttoLang import parity: Java / LLVM (M0)

This document describes the *checked-in contract and tests*, not a claim of
executed cross-backend parity.

## Implementation boundary

Portable library algorithms belong in `.atto` guest modules. The LLVM
compiler and C++ runtime implement parsing, import admission, type checking,
LLVM lowering, ownership and low-level privileged primitives. Do not substitute
a host-model implementation for guest compilation.

Java's canonical extension contract is
`oreslang-source.java/docs/ATTO_SOURCE_EXTENSION.md`: extensionless imports
prefer a present `.atto` before legacy `.ores`, with an explicit extension
authoritative and no unsafe fallback through a nonregular canonical source.

At this LLVM main-branch baseline,
`ProjectRootImport.hpp::resolve_native_source_suffix` still rejects
same-stem `.atto` plus `.ores` as ambiguous and can choose legacy `.ores`
when `.atto` is absent. This **differs from Java's documented precedence**.
Do not declare Java/LLVM import parity based on one happy-path compilation.
The staged canonical-source work in LLVM
[#1336](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/1336)
and manifest work in
[#1374](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/1374)
must be semantically reconciled, not overwritten with an older resolver.

## Executable M0 evidence gate

`native_atto_first_source_import_aot_parity` in CTest calls
`tests/atto_first_aot_import_parity.sh`. It:

1. Runs the real `oreslang-llvmc --target=host` on `main.atto` plus
   imported `math.atto` and requires generated LLVM IR.
2. Uses Clang at both `-O0` and `-O2`; each native executable must exit 42
   with empty stdout/stderr.
3. Repeats with explicit `math.atto` import while a conflicting same-name
   legacy `.ores` file exists, ensuring explicit canonical selection.
4. Requires missing, invalid, and directory-masquerading canonical
   dependencies to be rejected *without emitting any LLVM IR*.

The gate does not execute Java, prove Java's competing implicit-import
precedence, exercise actors/ownership/stdlib testing, or prove LLVM hard
real-time or sanitizer safety.

## Advancement criteria

- Record exact-head compiler and fixture Git blobs, actual nonzero executed
  CI steps, generated IR/guest exit receipts, and independent review.
- Reconcile LLVM `.atto`-first implicit resolution and canonical
  `.atto.pkg.toml` package-root admission with Java's fail-closed rules.
  Add positive tests with both suffixes present and negative tests for
  dangling links, directory entries, and package escapes.
- Execute identical portable `.atto` programs independently under Java
  JIT, Java Native Image, and LLVM O0/O2. Match semantic outcomes, not only
  source hashes. Extend to Wasm, Forge/Cranelift, and .NET separately.
- Treat zero-step GitHub Actions, host-only C++ models, and unexecuted source
  fixtures as **NOT EXECUTED**, never parity PASS.

The byte-identical Java linked-guest counterpart is [Java PR #2537](https://github.com/ores-truffle-oreslang/oreslang-source.java/pull/2537), which asserts guest exit 42 and explicit canonical-source selection. This relationship is source-level until both runners execute.
