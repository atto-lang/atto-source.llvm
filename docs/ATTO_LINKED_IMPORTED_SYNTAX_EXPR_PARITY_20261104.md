# LLVM imported Syntax<Expr> macro parity (bounded source-only M1 slice)

**Status (2026-11-04): DRAFT / AUTHORED / NOT_EXECUTED.**

This is stacked on LLVM [#1412](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/1412) → [#1407](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/1407) → [#1378](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/1378). No source crossed a GitHub organization.

## Semantic improvement

The native production parser already admits checked declarations of `Syntax<Expr>` identity and `quote(unquote(value) + unquote(value))` for *local* macro invocation, but imported public macro expansion previously permitted only `Const<int>` literal quoting. This slice adds bounded typed-AST expansion for the two importer forms **without** textual interpolation, guest macro symbol, host eval, dynamic JIT, or runtime source loading:

- `lib.identity!(syntax expr E)` forwards the existing parsed/qualified `E` AST **once** by move; preserves normal ownership and effect checking and does not evaluate `E` at expansion.
- `lib.twice!(syntax expr 21)` constructs two independent fresh integer AST leaves beneath a checked `Add`, only after proving the input is a single literal node and that doubling does not overflow signed i64. Variables, function calls, borrow expressions and composites fail closed.
- Existing public macro declaration/provenance/owner checks and 128-import-expansion budget remain in force. Wrong argument kinds fail before LLVM IR.

## Authored regression execution gates

`tests/native_module_imports.py` now has four additional **real compiler → LLVM IR → Clang → native guest 42** positive cases: imported identity(42), identity(40-producing call + 2), twice(21), and nested identity(twice(21)). The suite's existing harness removes source files before executing the compiled guest. Four added negatives require compiler rejection with **empty stdout (zero IR)**: runtime call duplication, variable duplication, signed-i64 overflow, and identity/wrong argument kind.

These real compiler/guest cases are authored, **not yet executed against a built compiler**. An isolated C++17 reproduction of the AST move/literal-twice logic passed seven checks under **GCC and Clang with ASan/UBSan**; it is not the production LLVM compiler, compiler CTest, or linked guest. The targeted `llvm-smoke` [run 41886621399](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/actions/runs/41886621399) was re-requested in the source organization and failed with **zero executed steps**. No JIT/AOT or five-backend PASS is claimed.

## Important remaining gaps

1. No general imported `Syntax<Expr>` AST/template composition, fresh hygienic identifier declarations, recursion, nonliteral repeated unquote, effects proof, or `SyntaxList<Decl>`. Unsupported forms must reject prior to native codegen.
2. Exact public-export artifact identity and cross-unit ABI semantics still differ from Java/Graal's broader compiler-only macro design. The 128-import budget differs from the Java 1,024 cap; harmonize or document as target-specific restricted profile.
3. The per-project logical source-ID / TOCTOU hardening in #1412 is separate and must receive independent native compiler/sanitizer tests.
4. Require immutable exact-HEAD GCC/Clang CMake + CTest, ASan/UBSan, guest exit=42, no runtime macro symbols, no unexpected stdout/IR, and independent security/compiler review before readiness or merge.
5. Unresolved source-org runner admission and private-source export governance block any source-bearing alternate-org CI. Source-free canaries cannot certify this code.

Related Java [#2504](https://github.com/ores-truffle-oreslang/oreslang-source.java/pull/2504), [#2529](https://github.com/ores-truffle-oreslang/oreslang-source.java/pull/2529), and linked [Java issue #2492](https://github.com/ores-truffle-oreslang/oreslang-source.java/issues/2492).
