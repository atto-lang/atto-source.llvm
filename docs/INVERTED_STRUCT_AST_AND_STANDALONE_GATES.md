# Native inverted-struct AST and standalone source-adapter gates

## Production source parser: nominal schema only

`Parser.hpp` now defines `InvertedStructDecl`, `InvertedStructField` and `InvertedScalarKind` and stores these separately from classes in `Program.inverted_structs`. Source parsing accepts either `define pub inverted struct` or `pub define inverted struct`, at root or inside a module, and records fields in declaration order with scalar type and mutability metadata.

Field examples: `x: int;` is implicit immutable `val`; `let y: f64;` has explicit mutability. This metadata is **not permission to lower or allocate**. `resolve_and_validate_program` deliberately rejects every nonempty inverted-struct catalog with a precise `typed SoA HIR/MIR ownership lowering is not implemented` error, regardless of the source parser's syntactic acceptance. Normal-mode `inverted class` remains rejected. One-level nested ordinary structs, fields with resources/pointers, methods and generational owner features are NOT accepted by this initial scalar-schema frontend.

Compiled AST CTest: `native_inverted_struct_schema_ast` builds the **real production Parser.cpp** (with its method/type/ownership dependencies and LLVM Support) and verifies root and module parsed declarations, `pub` order, ordered fields, implicit `val` vs explicit `let`, nominal value identity distinct from classes, duplicate and unsupported field rejection, and explicit no-codegen gate. It needs LLVM development headers/libraries and has not executed on the hosted source-org Actions runners.

## Independent C++20 adapter test for environments without LLVM-dev

```sh
cmake -S tests/inverted-i64-standalone -B build/inverted-i64-standalone \
  -DCMAKE_CXX_COMPILER=g++ -DBUILD_TESTING=ON
cmake --build build/inverted-i64-standalone --parallel
ctest --test-dir build/inverted-i64-standalone \
  --output-on-failure -R inverted_i64_standalone_source_to_native
```

The standalone target builds `tests/inverted_i64_standalone_driver.cpp` plus the **same** opt-in experimental source-adapter header used in `main.cpp`. It calls the exact checked C++ source compiler on actual `.ores` fixture files, emits LLVM IR and then exercises the existing `native_inverted_i64_guest.py` script via a native Clang driver.

This separate driver deliberately has only a bounded file read and an opt-in flag; it is **not** production `oreslang-llvmc`, does not establish the main compiler CLI dispatch or production file-identity/permissions contract, and is not a source-safety authority. Never report it as a full project build. The full LLVM project also registers `native_inverted_i64_guest` unconditionally when BUILD_TESTING is enabled, so a missing Clang fails visibly rather than silently omitting the test.

## Evidence and blockers

The current model's environment has CMake/GCC/Clang but **no LLVMConfig.cmake / LLVM development package**, so the full LLVM compiler and new production parser AST CTest cannot be built here. The standalone CMake target is staged for exact-head execution; it must obtain its own stepful test receipt before being promoted. Earlier locally verified exact-header i64-only guest fixture evidence remains scoped to the old experimental adapter blob, not to full production parsing or a new CTest run.

Do not mirror private Git trees to another GitHub organization until source branch protections, destination allowlist, independent review and reproducible SHA provenance are explicitly approved. Zero-step GitHub Actions failures are infrastructure evidence, not tests.

**Promotion order:** (1) stepful full exact-head parser AST and source adapter checks; (2) compiler-owned layout HIR/MIR and borrow-safe owner/row handles; (3) native checked row lowering; (4) LLVM→wasm32 and Java/.NET/Forge guest parity on identical shared fixtures; (5) AOT/sanitizer/real-time gates.
