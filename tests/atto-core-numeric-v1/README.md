# Canonical AttoLang numeric parity — LLVM candidate

This is an executable **candidate**, not a claimed passing compiler/guest result.

The Java/GraalVM branch in oreslang-source.java PR #2542 contains canonical freestanding `numeric.atto` with Git blob `5ced991f1d49b3c2a60fdd72b9003c0a4d18cd72`. This LLVM repo's `stdlib/numeric.atto` is **the same blob**. The compiler-owned native `kNumericSource` raw string must be byte-identical to it and is checked before any native result is accepted.

Three positive programs each expect OS exit 42 from real LLVM compiler → LLVM IR → mandatory no-GC auditor → Clang O0 and O2 → native guest (six native executions). The mixed case imports canonical, legacy and extensionless spellings into one linked program; canonical `.atto` must not create duplicate logical module identities. Two malformed suffix imports must be rejected without partial IR.

Use `bash tests/atto_core_numeric_parity.sh --source-only` for byte/source checks only and `bash tests/atto_core_numeric_parity.sh --run` for compiler-backed native evidence. The `--run` path publishes receipts only after all cases execute.

**Limitations:** `ores.std/core.atto` and `ores.std/assert.atto` are still unimplemented on LLVM at this parent branch; no hosted `Assert`, `Any`, `rt fail`, string/Unicode policy, or full standard-library parity is claimed. Source identity is not guest parity. Exact-head, nonzero-step CI, C++ registry tests, independent review, no-GC audit and execution are mandatory. Do not export private repo sources across organizations.
