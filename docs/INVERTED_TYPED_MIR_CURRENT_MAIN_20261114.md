# Inverted typed HIR/MIR — safe current-main forward-port (2026-11-14)

**Status: staged compiler-owned verifier and standalone native IR smoke only. This is not executable production `.atto` syntax.**

The older draft [LLVM #1403](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/1403) is no longer safely mergeable with current main, whose Git tree includes many newer parser/runtime files. This branch forward-ports only the reviewed typed schema/HIR/MIR and scalar IR emitter surface onto **current** main without replacing its newer parser, main driver, module loader, or CMake rules.

## Exactly what the forward-port adds

- `frontend/Parser.hpp`: inert `InvertedStructDecl`, `InvertedSourceOperation` and `InvertedSourceMain` descriptors, appended to Program to preserve older aggregate initialization order.
- `types/InvertedTypedPlan.hpp`: bounded typed schema, mutability, field/type/size admission.
- `types/InvertedTypedMir.hpp`: a checked sequence of allocation, field access, swap-delete, borrow and required terminal Close.
- `types/InvertedSchemaMirAdmission.hpp`, `InvertedProductionSourceMir.hpp`: independent HIR/MIR validation on a compiler-built Program.
- `types/InvertedMirLlvmI64.hpp`: narrow two-i64 LLVM IR emission, including row and ownership guards, terminal close/scrub.
- `tests/native_inverted_production_mir_test.cpp`: negative mutation and strict C++17 invariant checks, forward-ported from #1403.
- `tests/native_inverted_mir_codegen_smoke.cpp` and `.sh`: checked MIR to LLVM IR, Clang native exits 42 under both -O0 and -O2.
- `frontend/Parser.cpp`: source inverted class/struct declarations fail with `DOP_INVERTED_BACKEND_UNSUPPORTED`; directly constructed Program descriptors also fail through the **ordinary** resolver. This blocks accidental guest execution on an incomplete path.

## Explicit nonclaims

The smoke constructs a typed `Program` in C++. It does not read/parse a `.atto` program, nor build the entire `oreslang-llvmc` compiler. A passing source-free MIR and emitted native artifact is **not** full guest AOT, source parser acceptance, a general `inverted class`, a JIT/deopt test, actor-owned heap integration, no-GC proof for the whole binary, or cross-backend parity. The emitting path exists as a separately linked reference only; production codegen remains denied until admission is complete.

## Repro and gates

Run `bash tests/native_inverted_mir_codegen_smoke.sh` in a provisioned Linux compiler environment. The script uses GCC + Clang, compiler warnings as errors, Clang ASan+UBSan, checks the resulting IR, and verifies **actual** exit 42 at -O0 and -O2. It uses neither Python nor runtime GitHub secrets.

Before merging any source-enabling follow-up:

1. Preserve all present-day parser, module, method and actor behavior when moving the actual lexer/parser methods from the old draft. Build the entire production compiler on current main, not an obsolete branch.
2. Reconcile import/linker cross-unit rejection, unsupported mutations, class/struct identity, actor ownership, scope, move/borrow, and method dispatch before accepting source. Never silently drop an inverted AST.
3. Run normal compiled `.atto` entry points through default `oreslang-llvmc` to inspected native IR, then Clang -O0/-O2 real execution for both positive and expected bounds-trap fixtures; a positive exit-code stand-in is invalid.
4. Port the same canonical `.atto` fixtures to Java/Graal and compare semantic traces, including handles, borrowed columns, swap-delete, and teardown.
5. Require exact source tree SHA for full compilation; exact leaf blob verification only qualifies the isolated reference smoke. Current source-org Actions may report failed zero-step jobs; those are **NOT_EXECUTED**.

No source-org main branch or existing stacked feature PR has been modified by this forward-port.
