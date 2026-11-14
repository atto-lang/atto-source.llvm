# Main-based LLVM inverted i64 production extraction (draft)

## Scope and provenance
Source branch: `parity/inverted-production-main-i64-20261104`.
Base commit: `040058c1750a049be43949c41f61a87d028e6ddc`.
Related work: LLVM #1213 (diverged integration), #1385 (separate host dense-index substrate), conformance #249.

This source path admits exactly one `define inverted struct` with two i64
fields and one local literal-only `main` containing a bounded
`InvertedArray<T>.with_capacity(1..256)`, pushes, mutable-field sets,
`erase_swap` and a terminal field return. The narrow value-type
extension also admits exactly one `let saved = rows.snapshot(index);`
followed by a terminal `return saved.field;`. The capture copies both i64
columns to distinct stack-local cells; later source-row writes, swap-delete
and dense-index reuse cannot change the detached value. This does **not**
enable inverted classes or stable reference handles. Real native Parser/Program
AST is converted to typed HIR, independently verified MIR and checked
LLVM IR through `oreslang-llvmc`, **without an experimental CLI flag**.

The emitted owner is stack-local: LLVM checks owner liveness, current dense
length and positive live-row metadata; invalid reads trap; final close revokes
and volatile-scrubs columns, markers and detached snapshot cells. Guest code never obtains
owner-issued handles or raw host spans. Source-level loans, dynamic
expressions, non-i64 scalars, nested types, actors, await, imports, hard-RT,
Wasm, .NET NativeAOT, arbitrary receiver methods and inverted classes are
unsupported and must fail closed. Multiple snapshots, snapshot aliases,
exported snapshot pointers or unbound snapshot returns are rejected.

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
  oreslang-native-inverted-struct-ast-test
ctest --test-dir build -R \
  'native_inverted_(production_source_to_native|struct_schema_ast)|inverted_production_source_mir_cpp17' \
  --output-on-failure
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure -j 2
```

The native guest test compiles six local `.atto` positives plus one
canonical DOP fixture into LLVM IR and executes each at Clang O0 and O2
expecting exit 42 (14 native guest receipts). The detached snapshot fixture
captures a two-field struct, mutates and swap-deletes the original row,
then returns the **old captured value**. Two out-of-bounds guests must
terminate from a real SIGILL/SIGTRAP at both optimization levels.
Ten source-admission negatives (including unbound, duplicate, out-of-bounds,
class-as-value snapshots and attempted mutation of a detached value) and five unsupported compiler-mode negatives
must fail before writing any LLVM IR. Then run GCC/Clang sanitizer builds
(ASan and UBSan), compare against the exact-main baseline, and obtain
independent review.

Current receipt: **SOURCE_COMMITTED**. Production exact-head full
compiler, CTest, native guest, sanitizers and five-backend parity:
**NOT_EXECUTED**. GitHub pre-step failure is not a test failure.
No cross-organization private-source mirror was performed.
