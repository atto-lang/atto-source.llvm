# Main-based LLVM inverted i64 production extraction (draft)

## Scope and provenance
Reconciliation branch: `reconcile/inverted-production-main-20261114` ([draft PR #1812](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/1812)).
Current-main base commit: `3667b0255269ca30de381077435253f1b40be28f`.
Historical source: PR #1403; only its feature blobs were forward-ported onto the current compiler.
Related work: LLVM #1213 (diverged integration), #1385 (separate host dense-index substrate), conformance #249.

This source path admits exactly one `define inverted struct` with two i64
fields and one local literal-only `main` containing a bounded
`InvertedArray<T>.with_capacity(1..256)`, pushes, mutable-field sets,
`erase_swap` and a terminal field return. Real native Parser/Program
AST is converted to typed HIR, independently verified MIR and checked
LLVM IR through `oreslang-llvmc`, **without an experimental CLI flag**.

The emitted owner is stack-local: LLVM checks owner liveness, current dense
length and positive live-row metadata; invalid reads trap; final close revokes
and volatile-scrubs columns and markers. Guest code never obtains
owner-issued handles or raw host spans. Source-level loans, dynamic
expressions, non-i64 scalars, nested types, actors, await, imports, hard-RT,
Wasm, non-host NativeAOT targets, and the actor/hard-RT profiles are unsupported
and must fail closed. The supported narrow output is **host-native Clang AOT**
from verified local MIR, not general cross-target NativeAOT.

The `InvertedColumns` checked-handle host reference is **not** the storage
implementation for this bounded guest path. In particular the guest's
live-row bit is not a generational handle and must not be described as one.

## Required execution, not yet certified
Run on an authorized source runner, with exact checked-out PR SHA and
nonzero executed workflow steps:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --parallel 2 --target \
  oreslang-llvmc oreslang-inverted-production-mir-test \
  oreslang-native-inverted-struct-ast-test \
  oreslang-no-gc-ir-audit-build
ctest --test-dir build -R \
  'native_inverted_(production_source_to_native|struct_schema_ast)|inverted_production_source_mir_cpp17' \
  --output-on-failure
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure -j 2
```

The native guest test compiles four local `.atto` positives plus one
canonical DOP fixture into LLVM IR, then executes each at Clang O0 and O2
expecting exit 42 (ten native guest executions). The out-of-bounds guest
must terminate from a real SIGILL/SIGTRAP at both optimization levels.
Five source-admission negatives and seven unsupported compiler-mode negatives
must fail before writing any LLVM IR. Then run GCC/Clang sanitizer builds
(ASan and UBSan), compare against the exact-main baseline, and obtain
independent review.

Current receipt: **SOURCE_COMMITTED**. Production exact-head full
compiler, CTest, native guest, sanitizers and five-backend parity:
**NOT_EXECUTED**. GitHub pre-step failure is not a test failure.
No cross-organization private-source mirror was performed.

## Current-main command admission reconciliation

Current main additionally exposes `--isolate-entry` and explicitly records
`--target=host`. Neither is a supported modifier for this restricted
inverted-owner native `main`: both must be rejected **before IR output**.
The native guest harness includes negative tests for both options.
