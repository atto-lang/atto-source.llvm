# Inverted typed HIR/MIR — safe current-main forward-port (2026-11-14)

**Status: restricted two-`i64` production `.atto` source path staged; full compiler build and guest execution require independent verification. Other inverted forms remain unsupported.**

The older draft [LLVM #1403](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/1403) is no longer safely mergeable with current main, whose Git tree includes many newer parser/runtime files. This branch forward-ports reviewed typed schema/HIR/MIR and scalar IR emission onto **current** main and surgically integrates a bounded source parser path with the current driver and loader, without replacing those larger translation units or their unrelated behavior.

## Exactly what the forward-port adds

- `frontend/Parser.hpp`: inert `InvertedStructDecl`, `InvertedSourceOperation` and `InvertedSourceMain` descriptors, appended to Program to preserve older aggregate initialization order.
- `types/InvertedTypedPlan.hpp`: bounded typed schema, mutability, field/type/size admission.
- `types/InvertedTypedMir.hpp`: a checked sequence of allocation, field access, swap-delete, borrow and required terminal Close.
- `types/InvertedSchemaMirAdmission.hpp`, `InvertedProductionSourceMir.hpp`: independent HIR/MIR validation on a compiler-built Program.
- `types/InvertedMirLlvmI64.hpp`: narrow two-i64 LLVM IR emission, including row and ownership guards, terminal close/scrub.
- `tests/native_inverted_production_mir_test.cpp`: negative mutation and strict C++17 invariant checks, forward-ported from #1403.
- `tests/native_inverted_mir_codegen_smoke.cpp` and `.sh`: checked MIR to LLVM IR, Clang native exits 42 under both -O0 and -O2.
- `frontend/Parser.cpp`: accepts precisely one standalone two-`i64` inverted struct and literal-only `fnc main` with bounded `InvertedArray` push/set/erase_swap/get; independent MIR verification runs before codegen. Unsupported schemas/actor/class/linking combinations fail closed.
- `frontend/ModuleLoader.cpp`: refuses to drop an inverted schema or owner while linking multiple source units.
- `main.cpp`: emits the independently checked bounded MIR LLVM IR through default `oreslang-llvmc`; special RT/actor/target flags and other unsupported combinations remain rejected.
- `contracts/dop-inverted-v1/`: canonical `.atto` positive swap/reuse and negative removed-index trap fixtures pinned byte-identically to Java.

## Explicit nonclaims

The older smoke constructs a typed `Program` in C++, and **cannot** qualify as source execution. The new compiler parser/driver path is separately wired; its actual compilation and guest results must be reported from executed exact-source Actions, not inferred from host-model tests. A compiler-slice build is **not** a full source Git-tree build. No general `inverted class`, JIT/deopt, actor-owned heap, whole-binary no-GC, or cross-backend parity is certified by this restricted first step.

## Repro and gates

Run `bash tests/native_inverted_mir_codegen_smoke.sh` in a provisioned Linux compiler environment. The script uses GCC + Clang, compiler warnings as errors, Clang ASan+UBSan, checks the resulting IR, and verifies **actual** exit 42 at -O0 and -O2. It uses neither Python nor runtime GitHub secrets.

Before merging this draft:

1. Run the actual `Parser.cpp`, `main.cpp`, linker, ownership checker and native emitter in an independently built compiler. Preserve modern parser/module/method/actor behavior and negative admissions.
2. Keep the current restricted source grammar narrow: separate reviews must cover general inverted classes, handles, actor ownership, methods, escape/lifetime and cross-unit schema identity. Never silently drop an inverted AST.
3. Run normal compiled `.atto` entry points through default `oreslang-llvmc` to inspected native IR, then Clang -O0/-O2 real execution for both positive and expected bounds-trap fixtures; a positive exit-code stand-in is invalid.
4. Port the same canonical `.atto` fixtures to Java/Graal and compare semantic traces, including handles, borrowed columns, swap-delete, and teardown.
5. Require exact source tree SHA for full compilation; exact leaf blob verification only qualifies the isolated reference smoke. Current source-org Actions may report failed zero-step jobs; those are **NOT_EXECUTED**.

No source-org main branch or existing stacked feature PR has been modified by this forward-port.
