# AttoLang native test-harness parity: LLVM M0

## Evidence classification

**Scope: LLVM compiler-backed scalar guest execution subset, not Java TestKit parity.**

This draft implements native `*.atto` source admission, compiler-produced
LLVM IR, real Clang `-O0` and `-O2` linked executables, and observed native OS
exit statuses. The **24 positive** programs and **six negative** cases (three
checked-in compiler negatives, plus implicit legacy import, dangling canonical
import and unsupported `ores.std/testing`) are covered by the focused workflow.

The source-free inventory/mutation checks do not execute a compiler and are
not eligible as native guest evidence. The `ATTO_LLVM_NATIVE_HOST_OBSERVED`
lines come from a host-side test runner **after** executing native guest
binaries. They are deliberately **not** `ORES_TEST|PASS` guest receipts.
A passing count from this subset must never be reported as the Java native
`TestSuite`/filter/skip/fail-fast/actor harness passing on LLVM.

### Portable execution contract

| Contract | Java / GraalVM | LLVM at this draft |
| --- | --- | --- |
| Canonical `.atto` input | compiler-integrated | compiler-produced LLVM IR |
| Scalar expressions / direct calls | supported by source model | 24 native executable cases |
| Explicit and extensionless `.atto` imports | canonical source | included executable cases |
| `.atto.pkg.toml` `+/...` import | canonical package | one native executable plus C++ security tests |
| Canonical/legacy peer precedence | `.atto` wins; legacy explicit | `.atto` wins; `.atto` caller requires explicit `.ores` |
| Native `std/testing.atto` module | authored in Java compiler | **UNSUPPORTED: explicitly rejected** |
| `TestCase`, `TestSuite`, `RunSummary` | authored in AttoLang stdlib | **NOT IMPLEMENTED** |
| Guest-created `ORES_TEST` machine events | Java workflow requires receipts | **NOT IMPLEMENTED** |
| Actor probes, virtual clock, cancellation | proposals only | **NOT IMPLEMENTED** |
| LLVM O0/O2 native guest execution | separate runtime | required by this LLVM workflow |

### Exact-runner acceptance

- Checkout exact PR head, record complete tree SHA, reject modified tracked
  files before/after; no private cross-organization mirror.
- Execute `bash tests/test_atto_native_parity_manifest.sh` and reject all
  source-free inventory mutations. These checks never increment guest count.
- Build **real** `oreslang-llvmc` via CMake; execute updated C++ project-root
  import security tests.
- For each of 24 reviewed `.atto` sources, run the compiler twice to emit
  nonempty LLVM IR defining `@main`, compile independently at Clang `-O0`
  and `-O2`, execute both binaries, assert exact native OS exit code, ensure
  no unexpected stdout/stderr, and compare independently emitted IR.
  This yields **48 compiler-backed native executions** only if steps actually run.
- All six unsupported/invalid cases must produce nonzero compiler exit,
  diagnostic stderr, **no partial emitted IR**, no timeout or runner abort.
- Guard the manifest, package marker, and positive/negative AttoLang guest
  sources against mutation during execution. Machine-readable markers
  `ATTO_LLVM_NATIVE_HOST_OBSERVED` and
  `ATTO_LLVM_NATIVE_PARITY_SUBSET_EXECUTED` identify observed exits only.

The portable subset also includes a source-level AttoLang scalar assertion
module `scalar-assert-lib.atto`. Its passing guest returns 0 and its deliberate
failed assertion returns 1; both outputs are tested as *observed native exits*
under Clang O0 and O2. This does **not** implement Java `TestCase`,
`TestSuite` or guest-emitted receipt semantics.

The tests are registered in CTest only on Linux as two independent targets:
`atto_native_harness_inventory_source_only` and
`atto_native_harness_compiler_guest_o0_o2`. The standard CTest target must
build and invoke the real AttoLang LLVM compiler and Clang; it cannot pass by
running the source-only manifest guard alone. macOS/Windows support is not
claimed for this GNU-timeout/bootstrap runner.

The native compiler's OS-exit ABI is a bounded `0..255` result. Java's test
framework uses guest-produced per-case machine receipts. These are **different
reporting abstractions**; do not equate the 24 LLVM programs with the 132
authored Java TestCase registrations.

### Important remaining gaps

- The compiler-owned native `ores.std/testing` path is currently rejected.
  Implement first-class `stdlib/testing.atto` support under LLVM as a
  separate compiler/runtime task with real machine reporting and fail-closed
  negative guest evidence. Do **not** generate `ORES_TEST|PASS` from a
  shell script and call that guest emission.
- Add native skipped/filtered/fail-fast/error fixture semantics after real
  TestSuite lowering. Maintain exact output order, escaping, and nonzero exit
  on failed/empty test runs.
- The new canonical `.atto.pkg.toml` selection is a **minimal manifest
  reader**. It accepts `schema_version = "1"` and safe literal
  `[project] root = "."` but refuses other project roots requiring full
  TOML lowering. A full Java-equivalent package manifest parser, pinned
  no-follow manifest file reads, and concurrent rewrite protection still
  require independent engineering and audit. Host-side security tests alone
  do not establish equivalence.
- Coordinate with LLVM #1306/#1337/#1345 migration branches before merging
  into `main`. Their overlapping native import/codegen files require
  semantic reconciliation, not blind force pushes.
- Zero-step private GitHub Actions runs are **NOT_EXECUTED**, not proof of
  compilation failure or parity. Require independent review and actual
  exact-head compiler execution before merge.

Run locally with a built production compiler (the source-free gate is
separate and cannot count as guest execution):

```bash
bash tests/test_atto_native_parity_manifest.sh
cmake -S . -B build -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2 --target oreslang-llvmc oreslang-native-project-root-import-test
ctest --test-dir build -N
ctest --test-dir build --output-on-failure --timeout 120 \
  -R '^native_project_root_import_security$'
ORESLANG_LLVMC="$PWD/build/oreslang-llvmc" CLANG="$(command -v clang)" \
  bash tests/atto_native_harness_parity.sh --run
```

### Java test harness comparison / November 2026

The Java AttoLang stack is tracked in Java PRs
[#2494](https://github.com/ores-truffle-oreslang/oreslang-source.java/pull/2494)
and [#2495](https://github.com/ores-truffle-oreslang/oreslang-source.java/pull/2495).
It authors an `ores.std/testing` implementation, 132 registered native test
cases in six manifest-backed suites, 40 ordered machine receipts and four
deliberately failing guests. LLVM **does not yet implement that module**.
The present LLVM subset only verifies 19 independent source programs at two
Clang optimization settings, with a host-observed exit marker. The negative
`ores.std/testing` admission test must continue to reject until LLVM actually
has library parsing, lowering, and guest-produced per-case receipt emission.

Do not report **Java TestKit parity** based on shell output markers, shared
spec documents, source-free checks, or a failed private GitHub Actions run.
The full requirements include compiler-backed real `.atto` guest execution,
the same ordered test IDs, exact error accounting, failure/skipped/filtered
semantics and explicit nonzero exit for invalid suites. Private hosted CI jobs
have experienced zero-step admission failures; check job steps before claiming
any executable parity.

The focused LLVM workflow uses a **single** source-free gate, a real native
compiler build, C++ import security checks, 38 potential guest executions
(24 sources × two Clang optimization levels), and six potential negative
compiler admissions. No cross-organization private source transfer is used.
