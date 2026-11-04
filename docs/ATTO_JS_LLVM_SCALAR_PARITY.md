# AttoLang JS ⇄ LLVM scalar parity gate (M0)

This draft adds two independently labeled native-execution matrices for **the current native LLVM compiler**. It is intentionally not a claim of full Java/Graal, LLVM, Wasm, Cranelift or .NET parity.

## Canonical source identities

| Scope | Corpus path | Git blob SHA-1 | SHA-256 |
| --- | --- | --- | --- |
| Shared scalar50 | `contracts/atto-parity-v1/shared-scalar50-v1.tsv` | `a948dbaf594916a6ee07f65d49a0cf5ae398ca7b` | `210dbe7363e5388e4f82c4de9fe7f8136ee6a7dca698ce72165e006fd5ecb0db` |
| JS checked-i64 50 | `contracts/atto-parity-v1/js-checked-i64-candidates.json` | `9f3e54586a71ae4e5525e308b26c61ea70bc7232` | `5fd8ed522dfaaead8dc1b2d780159a4f156482124b7515d0a788245fc594ebc8` |

The first corpus is byte-identical to the JavaScript `shared-scalar50-v1.tsv` fixture, which itself pins the Java shared source corpus. The second is byte-identical to the checked-i64 candidate manifest in [JS PR #111](https://github.com/ores-truffle-oreslang/oreslang-source.js/pull/111). These are test fixture bytes, not a compiler/runtime source mirror; no cross-organization transfers were made.

## M0 — 50 ordinary scalar source programs

`atto_shared_scalar50_native.cmake` first rejects any changed manifest hash, wrong ID order, invalid source profile, missing case or unrepresentable process exit. Its *native* mode then:

1. Writes each exact source field to `*.atto` without adding an extra newline.
2. Runs the actual built `oreslang-llvmc` to produce bounded LLVM IR.
3. Uses Clang to link that IR into a native executable.
4. Executes all 50 native binaries, requiring the exact expected process exit and empty stdout/stderr.
5. Writes a successful receipt only after all 50 native executions; source HEAD/tree and compiler/Clang SHA-256 are captured.

The separate *host-only* validation test performs no compiler/guest execution and **must not be promoted as parity proof**.

## M1 — 37 full-width signed-i64 values and 13 checked overflow traps

`atto_checked_i64_native.cmake` pins and validates the JS checked-i64 corpus.

For each of the 37 expected values, it retains the original expression bytes but wraps the expression in AttoLang `if ((expr) eq (expected)) { return 42; } fi`. This lets the compiled LLVM guest compare **the full signed-i64 value**, without losing information by truncating a native process exit status to eight bits. The resulting native source differs from the JS program and is separately hashed; the original JS source is also hashed.

For 13 expected overflows, the **complete original source program bytes** are compiled unchanged as `.atto`. The IR must contain checked-overflow and `llvm.trap` instructions. The guest must terminate with a recognized native trap signal, rather than an arbitrary nonzero exit. No source-free oracle, host-only trap simulation, compiler diagnostic, skip or timeout is counted as an executing native guest result.

This v1 gate supports Linux x86-64 / equivalent Unix trap reporting only; other platforms need reviewed trap normalization. It does not cover every statement, generic instantiation, actor or scheduler semantic.

## Running the gates

On an authorized source checkout with LLVM libraries, Clang and CMake installed:

```bash
cmake -S . -B build -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --target oreslang-llvmc --parallel 2
ctest --test-dir build --output-on-failure -R '^atto_(shared_scalar50|checked_i64)_'
```

Run the dedicated exact-head workflow `.github/workflows/atto-scalar50-native.yml` for PR-head checkout, compiler and Clang identity, and both JSON receipts.

## Evidence classification and release blockers

- Locally, the scalar50 manifest is verified by SHA-256 and a host-only CMake validation. Tampering and a missing compiler were correctly rejected. A fake-compiler/Clang experiment exercised the harness but **is not LLVM execution evidence**.
- The LLVM source organization currently fails GitHub Actions jobs **before a runner executes any steps** (see [runner issue #159](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/issues/159)). Do not interpret a zero-step failure as a compiler failure or a parity pass.
- LLVM PR #1363 must remain draft pending a stepful exact-head compiler/guest run, per-case receipts, a review of any actual failures, and independent approval.
- Full Java/LLVM compiler, ownership, actor, structured-concurrency and backend parity remains out of scope for these scalar gates.

The chosen fixed test source programs avoid the legacy remainder `%` operator; `.ores` remains supported for compatibility while `.atto` is used for these new native guest files.
