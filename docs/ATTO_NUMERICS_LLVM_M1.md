# AttoLang numerical LLVM M1 — typed scalar integration (draft)

## Source provenance and scope

The scalar parser now writes the resolved variable and field type back to each expression node before LLVM lowering, preventing stale default integer annotations on f64 reads. The native code generator also rejects non-`double` LLVM operands in f64 arithmetic and comparisons before emitting malformed IR. This is a defensive type invariant; it is not a substitute for source type checking.

This branch starts from LLVM main commit `e816303202c098f122c684320072005eea053467` and reconciles the C++ typed-f64 slice from [LLVM PR #1236](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/1236) with the merged canonical `.atto` import/runtime code from [PR #1312](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/1312).

It preserves the newer lexer fail-closed decimal-suffix rejection, and extends lexical `float` spelling to map to internal native `ScalarType::F64` without any implicit int/bool conversion. `f64` remains an admitted explicit synonym for the narrow native lexical subset. Ordinary synchronous scalar functions pass f64 values as LLVM double arguments and results. Calls preserve their checked source type across named arguments, branch returns, and linked source units. The process entrypoint retains its integer/void ABI and f64 parameters are by value. Numerical arrays and branch-local f64 storage have separate lowering gates.

## Floating-point model operations

The host compiler admits linear `define model` chains of pure arithmetic
functions over `int` or `float`. Adjacent scalar ports and configuration
arguments must match exactly. Floating-point literals, including signed
zero, bind their binary64 bits into the blueprint or configured digest.
The shared integer model profile and its existing digest vectors retain
their meaning. Float models currently use the native host path.

Typed tensor ports, tensor storage lowering, and GPU placement have
separate admission and execution gates.

## Native compiler evidence required

CTest `native_f64_scalar_e2e` uses `tests/native_f64_lexical_gate.sh` on actual canonical `.atto` source files. It requires a real `oreslang-llvmc` to emit nonempty LLVM IR, Clang to link it, and the resulting executable to exit its case-specific expected status without stdout/stderr for **28 positive scalar fixtures** (including 42, 86, 17, 29, 37 and 95 exits). **17 negative fixtures** must be rejected with source-error exit 1, a matching precise diagnostic and **zero LLVM IR**, not a crash or missing compiler. The five numerical prerequisites cover negative-domain, tiny/huge magnitudes, finite-input overflow to infinity, and addition of four scalar floats. Every positive fixture must be compiled to an actual native executable **twice** from the same compiler-emitted IR, at Clang `-O0` and `-O2`, for **56 expected native executions**. The required per-case exit status and empty stdout/stderr must hold at both optimization levels. A compiler-only pass or O0-only result is insufficient. One positive and one negative fixture cover `float` source aliasing. Five divergent-exit positive guests catch any compiler path that merely emits a constant 42 program. Two additional positive guests exercise nested and repeated f64 variable reads rather than only literal operands. The harness rejects duplicate or unsafe test-case IDs (including across positive and negative groups) before invoking the compiler. The negative fixtures also reject source alias `float` at unsupported function ABI boundaries and preserve the exact Decimal suffix denial.

CTest `native_orie_stdout_alias` separately requires real source-to-native compilation and exact piped stdout through the runtime's nonblocking output path.

The local arm64 macOS reconciliation ran the native scalar matrix at both optimization levels with LLVM 20 and Clang. Source-organization Actions and the complete numerical library require independent execution evidence.

## Full locked numerical-suite admission (separate gate)

[numerics-conformance PR #6](https://github.com/numerics-atto/numerics-conformance/pull/6) locks the seven real `.atto` source files, all 16 case names, and `numerics-atto/num.atto` Git commit `f7585fc26478ffd22696c1b05c74c17d8244380c`.

The `scripts/run-llvm-m1-guest.mjs` adapter in that **separate** private repository requires the locked source tree and SHA-pinned real LLVM compiler, AOT linker script, IR auditor and Clang. It invokes the native AOT path and checks all 16 actual stdout test records; its receipt is `LLVM_EXECUTED_CANDIDATE_NOT_ATTESTED` until independent provenance review.

**Not implemented by this scalar patch:** full `Result<float,String>`, `List<float>`, source-level `sqrt`, deterministic reductions on lists, matrix layouts, SoA particle energy, and full numerical test harness execution. Do not promote any of the 16 `num.atto` fixtures to LLVM PASS based on the 28 scalar source probes, even if their O0/O2 executions pass.

## CI and security gates

Source-org GitHub Actions failures with null/empty steps are runner-admission failures, not compiler test results. Do **not** mirror private LLVM compiler source into another organization without independently approved destination ID, protected source authority, exact-tree proof and verified ref/object cleanup. Keep this PR draft and request independent review. After runner admission, build full LLVM C++/runtime, execute the focused CTest positives and negatives, retain exact compiler and Clang digests, verify no IR for negatives, then run the full seven-file numerical M1 fixture and independently compare Java.

## Source-to-IR and native-image admission hardening (November 4, 2026)

The native f64 CTest now refuses a source-blind compiler stub that emits only an integer `main`. Every positive floating-point fixture must contain a **real LLVM `double` instruction** (alloca/load/store/floating operation) rather than a comment pretending to be floating IR; the dedicated integer control is the only exception. A real `i32 @main` definition is required. The test requires valid native linked-image magic (ELF, Mach-O or PE) rather than accepting shell/Node wrappers, verifies fixture/source and IR SHA-256 identities before and after *each* native guest, and verifies that the linked image is unchanged after execution. Test source, exit status and expected diagnostics have explicit size/range budgets. SHA-256 output supports GNU `sha256sum` on Linux or `shasum -a 256` on macOS.

The separate `native_f64_harness_adversarial_host_only` CTest, also invoked as the CI Bash preflight, uses **six fake-compiler/host-binary negative mutations**: fabricated double IR comments, wrong native main, intentional linker failure, script shim, actual host C constant-result executable, and linker-stage mutation of source IR. Every mutation must fail closed; `HOST_ONLY_F64_GATE_ADVERSARIAL_PASS=6` is **not** compiled AttoLang, a native AttoLang guest, a numerical library test, or LLVM–Java parity.

**Execution evidence limitation:** none of this source-local hardening has yet run in the blocked LLVM source-org GitHub Actions environment. A local, source-free host probe verified example ELF magic versus script magic and the anchored typed-double regex, but not the full harness or the AttoLang compiler. Keep the PR draft until source-owned exact-head CMake/CTest runs with nonzero steps and independent review.

## Exact-head runner admission canary

The focused `.github/workflows/llvm-f64-numerics-m1.yml` workflow now runs two isolated jobs: a source-free shell canary (no source checkout, empty token permissions), and a read-only compiler-backed CMake/CTest job inside the same source organization. Each must execute nonzero steps before it contributes evidence. The CMake job performs `bash -n` and strict positive/negative source-case counts before building.

At head `e0412775869db5be2533d1d3b7a2818528c3d160`, [Actions #41767276770](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/actions/runs/41767276770) reported **both jobs failed with `steps=null`**. The source-free canary's failure makes ordinary compilation errors an insufficient explanation for the current hosted failure. It does not establish whether the exact root cause is exhausted minutes, billing restrictions, organizational policy or runner availability. There is still no executed LLVM compiler, no source-to-native fixture verdict and no Java/LLVM full numerical parity receipt.
