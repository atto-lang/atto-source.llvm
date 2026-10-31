# Inverted typed MIR to checked native owner — experimental bridge

## What is implemented

The new **InvertedTypedMir.hpp** lowers a compiler-owned LinearPlan to a typed MIR Module. A separate verifier checks allocation identity, one-level schema and field kinds, immutable member writes, field loan conflicts, unmatched releases, structural deletion and closure, suspension denial, and mandatory owner/generation/bounds obligations. Source row indices are checked for signed-i64 representability.

The restricted **execute_native_i64_pair** backend interprets this MIR over the real DopInvertedColumns<int64_t,int64_t> runtime. Typed push/write operands are separately supplied; the MIR does not expose guest pointers or raw native spans. An erased row is resolved through a generation-aware native row capability. The interpreter correctly supports multiple logical readers with one physical read loan per lane.

**Scope:** precisely two signed i64 columns. This is NOT the production LLVM IR generator, actor-owner pinning, a hard-real-time allocator, or a general Oreslang source compiler. Boolean, f64, packed nested structs, class methods, arbitrary CFG/await and all non-LLVM backend execution are unsupported here.

## Test and verification boundaries

tests/native_inverted_typed_mir_test.cpp is registered as CTest inverted_typed_mir_native_columns in standalone and full CMake. It checks push/read/write, owner/field loan effects, swap deletion, detached snapshots across owner close, shared physical read loans, and forged MIR guard/immutable field/close/overflow conditions. The exact-head Actions workflow builds GCC and Clang C++20 in ordinary and ASan/UBSan configurations with read-only permissions and SHA-checked PR-head checkout.

A LOCAL syntax smoke compiled a similar prototype using simplified **stand-in** HIR and runtime interfaces under GCC/Clang. This is not compiled execution of the repository's actual runtime bridge. The current-head four-job Actions matrix failed with ZERO steps at runner/admission, so actual real-runtime CTest, sanitizer and full oreslang-llvmc execution remain NOT_EXECUTED. The earlier source-adapter exit-42 results belong to a different older blob and are not transferred to this implementation.

The MIR guard flags record obligations, not proof that production LLVM code generation materialized the guards. The normal compiler must keep its fail-closed inverted source gate until a complete backend lowering implements checked owner+row validity and borrow-safe lifetime.

## Next gates

1. Run exact-HEAD C++20 standalone and full LLVM compiler CTest with real repository headers, both GCC and Clang, plus ASan/UBSan.
2. Implement codegen from verified MIR into explicit LLVM IR checked owner/row ABI; prove guards are emitted before every access.
3. Add CFG/dataflow loan joins, task pinning and physical quiescence, then Java parity and the shared backend guest fixtures.
