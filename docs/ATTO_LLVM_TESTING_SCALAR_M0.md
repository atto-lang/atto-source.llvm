# AttoLang LLVM native scalar assertion library — M0 (draft)

## Contract

This slice adds a **compiler-owned** AttoLang source module backed by the
canonical checked-in `stdlib/testing_scalar.atto` source. It is a small
portable **scalar assertion** surface, not a replacement for Java's complete
`stdlib/testing.atto` or a claim of guest-generated test receipts.

Exact canonical compiler import aliases:

- `ores.std/testing_scalar`
- `ores.std/testing_scalar.atto`
- `std/testing_scalar`
- `std/testing_scalar.atto`

All aliases select **one internal unit identity**
`@ores.std/testing_scalar.atto`. The compiler embeds the exact `.atto`
source bytes; it does not read a project-controlled file when resolving
compiler-owned standard-library imports. `std/testing_scalar.ores` is **not**
an alias. Unknown `std/*` and `ores.std/*` imports, including the full
`ores.std/testing`, are still rejected and must never fall through to
ambient project import roots.

The module exports:

| Native function | Result |
| --- | --- |
| `equal_int(actual, expected)` | `0` if equal, otherwise `1` |
| `not_equal_int(actual, expected)` | `0` if unequal, otherwise `1` |
| `combine_status(first, second)` | `0` if both are zero, otherwise `1` |

Integer result `0` means the test assertion passed; any nonzero result
must be **explicitly returned from the guest `main`** so the host can
inspect a native OS exit code. There is **no hidden test registry**, no
reflection, no Java host callbacks, no implicit coercion, no `TestCase`,
no timers or actor task scheduler, and **no `ORES_TEST` guest output**.
This M0 module is deliberately hosted-only; a freestanding native
compiler target must fail before admitting a hosted `ores.std/*` library.

## Regression and native execution gates

A dedicated source-free gate
`tests/test_atto_testing_scalar_contract.sh` byte-compares the checked-in
`.atto` source with the compiler's embedded raw module, checks aliases,
exact unit identity, hosted-only tier and fail-closed unsupported full
TestKit behavior, and rejects three targeted source mutations.

The compiled C++ `native_core_library_registry_provider_policy` CTest
also tests module identity, aliases, compiler-owned tier dependency fences
and exact checked-in `.atto` source fidelity. It is **host testing**, not
guest execution.

The existing focused native harness PR #1365 is the parent. This slice
extends the existing 24 canonical guest programs by **six**, for **30**
positive `.atto` entries. The same production compiler/Clang pipeline
must emit and run each at **O0 and O2**, giving **60 possible native guest
executions** only when every step actually runs. The manifest has **five**
checked-in negative compiler cases and the harness creates three
additional security negatives (**eight total**). New negative programs
ensure a legacy `.ores` suffix and unknown `ores.std` module do **not**
accidentally select this library. The previously existing unsupported
`ores.std/testing` check must continue to fail with no partial LLVM IR.

All negative results require actual compiler execution, a nonzero status
that is not timeout/kill, matching diagnostics and zero emitted partial IR.
A shell printout of an `ORES_TEST|PASS` marker would be forged evidence
and is forbidden. Host-observed native exit codes are not
guest-produced machine reporting semantics.

### Local command sequence (once built and authorized)

```bash
bash tests/test_atto_testing_scalar_contract.sh
bash tests/test_atto_native_parity_manifest.sh
bash tests/test_atto_native_workflow_contract.sh
cmake -S . -B build -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2 --target oreslang-llvmc oreslang-native-project-root-import-test oreslang-core-library-sources-test
ctest --test-dir build --output-on-failure --timeout 120 \
  -R '^(native_project_root_import_security|native_core_library_registry_provider_policy)$'
ORESLANG_LLVMC="$PWD/build/oreslang-llvmc" CLANG="$(command -v clang)" \
  bash tests/atto_native_harness_parity.sh --run
```

## Not established

The source-owning private GitHub Actions organization has historically
reported failures with **zero job steps**. Do not call this module
compiler-backed or merge-ready without **an admitted exact-head native
LLVM compiler build, 60 independently launched guest binaries, all eight
compiler-negative results, unchanged source tree and independent review**.
No private source is copied to another GitHub organization. CI, JIT/AOT
and Java/LLVM full TestKit parity remain unverified until real execution.

Subsequent work toward full AttoLang parity requires a typed
`TestCase`/`TestSuite` representation, registered callbacks under
ownership rules, native guest printing with bounded/escaped machine
receipts, skipped/filtered/fail-fast accounting, deterministic
scheduler/actor test support, and exact matching Java and LLVM guest
observations. Do **not** silently promote this M0 scalar-only module to
`ores.std/testing` until those functions are implemented and tested.
