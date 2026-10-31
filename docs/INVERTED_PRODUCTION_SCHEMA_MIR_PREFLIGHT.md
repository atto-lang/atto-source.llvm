# Production inverted-struct schema AST to verified MIR preflight

## Current implementation (draft PR #1213)

The real production `Program` AST now passes nominal inverted-struct declarations to `dop::mir::preflight_inverted_schemas(program)` in `src/main/cpp/oreslang/types/InvertedSchemaMirAdmission.hpp`.

Preflight is **C++17 compiler-only**, shares the same `InvertedTypedPlan.hpp` and `InvertedTypedMir.hpp` representations as the experimental native/backend work, and validates:
- 1..64 fixed-width columns per schema; i64/bool/f64 kinds; implicit immutable `val` vs explicit `let` metadata and canonical field order
- A budget of at most 64 inverted declarations per parsed Program
- Unique nominal inverted schema identity and collisions against class, actor and type-only declarations
- A **synthetic capacity-1 schema probe** with verified MIR instructions `Allocate` then `Close`, no field reads/writes, row handles, guest allocations, loans, emitted LLVM IR, or compilation authority

That final point is critical: the probe only establishes that the **production parser's layout metadata** is compatible with typed MIR shape validation. It is **not** production source-operation lowering. The production `resolve_and_validate_program` still throws `native inverted struct schema parsed but typed SoA HIR/MIR ownership lowering is not implemented` before any codegen. Unknown syntax/classes/nested or resource-bearing members remain rejected. The opt-in two-i64 source adapter continues separately.

## Tests and exact-head gate

New strict C++17 `tests/native_inverted_schema_mir_preflight_test.cpp` verifies verified-MIR probe instructions, widths/mutability, class/actor/type-only identity collisions, duplicates, unsupported field kind, declaration budget and empty program. Registered as `inverted_production_schema_mir_preflight` in both standalone and full CTest. Existing **real-parser** `tests/native_inverted_struct_ast_test.cpp` now exercises the preflight on a genuinely parsed mixed i64/bool/f64 schema and verifies no guest operations leak.

The exact-head GCC/Clang workflow triggers on the new schema header, both tests and parser changes. Current commit and source identities must be checked at the time an actual runner begins, not inferred from staged CTest names.

## Not yet proved

The private Git tree is unavailable for network cloning in the current local container; `llvm-config`, `LLVMConfig.cmake` and the full LLVM SDK are unavailable. The new strict C++17 tests and full production parser have **not executed** in this local environment. Hosted GitHub Actions jobs currently queue and/or fail before any steps; zero-step failures are **NOT_EXECUTED**, not evidence of either compilation success or a C++ regression.

**Promotion gates**: obtain stepful exact-head compiler and CTest build (GCC and Clang, ASan/UBSan), then add proper production source expressions and checked owner operation operands to the production AST/HIR; lower real source operations into verified MIR and LLVM codegen, with row-handle generation and borrower/lifetime validity, before removing the production fail-closed gate. No five-backend parity or zero-GC/real-time claim is made here.
