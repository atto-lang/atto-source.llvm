# AttoLang numerical LLVM M1 — typed scalar integration (draft)

## Source provenance and scope

The scalar parser now writes the resolved variable and field type back to each expression node before LLVM lowering, preventing stale default integer annotations on f64 reads. The native code generator also rejects non-`double` LLVM operands in f64 arithmetic and comparisons before emitting malformed IR. This is a defensive type invariant; it is not a substitute for source type checking.\n\nThis branch starts from LLVM main commit `e816303202c098f122c684320072005eea053467` and reconciles the C++ typed-f64 slice from [LLVM PR #1236](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/1236) with the merged canonical `.atto` import/runtime code from [PR #1312](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/1312).

It preserves the newer lexer fail-closed decimal-suffix rejection, and extends lexical `float` spelling to map to internal native `ScalarType::F64` without any implicit int/bool conversion. `f64` remains an admitted explicit synonym for the narrow native lexical subset. Parameters/returns of f64 functions, branch-local ownership and numerical arrays remain rejected until lowering exists.

## Native compiler evidence required

CTest `native_f64_scalar_e2e` uses `tests/native_f64_lexical_gate.sh` on actual canonical `.atto` source files. It requires a real `oreslang-llvmc` to emit nonempty LLVM IR, Clang to link it, and the resulting executable to exit its case-specific expected status without stdout/stderr for **25 positive scalar fixtures** (including 42, 86, 17, 29, 37 and 95 exits). **16 negative fixtures** must be rejected with source-error exit 1, a matching precise diagnostic and **zero LLVM IR**, not a crash or missing compiler. The five numerical prerequisites cover negative-domain, tiny/huge magnitudes, finite-input overflow to infinity, and addition of four scalar floats. One positive and one negative fixture cover `float` source aliasing. Five divergent-exit positive guests catch any compiler path that merely emits a constant 42 program. Two additional positive guests exercise nested and repeated f64 variable reads rather than only literal operands. The harness rejects duplicate or unsafe test-case IDs (including across positive and negative groups) before invoking the compiler. The negative fixtures also reject source alias `float` at unsupported function ABI boundaries and preserve the exact Decimal suffix denial.

CTest `native_orie_stdout_alias` separately requires real source-to-native compilation and exact piped stdout through the runtime's nonblocking output path.

These are authored targets **NOT EXECUTED** until a job records nonzero executed steps at the exact branch SHA with a real compiler and Clang. No result, performance claim, or Java/LLVM parity is asserted from source changes.

## Full locked numerical-suite admission (separate gate)

[numerics-conformance PR #6](https://github.com/numerics-atto/numerics-conformance/pull/6) locks the seven real `.atto` source files, all 16 case names, and `numerics-atto/num.atto` Git commit `f7585fc26478ffd22696c1b05c74c17d8244380c`.

The `scripts/run-llvm-m1-guest.mjs` adapter in that **separate** private repository requires the locked source tree and SHA-pinned real LLVM compiler, AOT linker script, IR auditor and Clang. It invokes the native AOT path and checks all 16 actual stdout test records; its receipt is `LLVM_EXECUTED_CANDIDATE_NOT_ATTESTED` until independent provenance review.

**Not implemented by this scalar patch:** full `Result<float,String>`, `List<float>`, float function parameters/returns, source-level `sqrt`, deterministic reductions on lists, matrix layouts, SoA particle energy, and full numerical test harness execution. Do not promote any of the 16 `num.atto` fixtures to LLVM PASS based on 20 scalar probes.

## CI and security gates

Source-org GitHub Actions failures with null/empty steps are runner-admission failures, not compiler test results. Do **not** mirror private LLVM compiler source into another organization without independently approved destination ID, protected source authority, exact-tree proof and verified ref/object cleanup. Keep this PR draft and request independent review. After runner admission, build full LLVM C++/runtime, execute the focused CTest positives and negatives, retain exact compiler and Clang digests, verify no IR for negatives, then run the full seven-file numerical M1 fixture and independently compare Java.
