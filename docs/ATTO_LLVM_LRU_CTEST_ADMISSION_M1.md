# LLVM LRU CTest execution-admission gate (source-free M1)

## Verified defect

CTest's `WILL_FAIL TRUE` inverts the success criterion: an actual failing
command can become a **Passed** CTest, even when the JUnit report says
`status="run"`, `failures="0"`, and contains no skipped child. A real
temporary CMake/CTest project reproduced this behavior. The existing
JUnit receipt gate alone cannot distinguish that case from a genuine
successful AttoLang guest.

## Defense

Before running the four LRU native CTests, source-local CI now records
`ctest --test-dir build/atto-lru --show-only=json-v1` and validates
that CTest's **configured executable commands** and **properties** are
exactly the reviewed five cases.

`tests/atto_lru_ctest_admission.py` verifies:

- Exactly four named native guest commands and one source inventory
  command, with no duplicates or unknown `atto_lru_*` tests.
- The expected Python interpreter, real `tests/contract.py` harness,
  pinned `.atto` fixture path, expected guest exit (42/17/29/61),
  built `oreslang-llvmc`, and resolved Clang executable.
- Correct `NativeIntArrayRegion.c` linkage on the three `Array<int>`
  programs and no unexpected linkage for quotient arithmetic.
- Reviewed CTest working directory and source-only `TIMEOUT 20`.
- **No** `WILL_FAIL`, `DISABLED`, `SKIP_RETURN_CODE`,
  `SKIP_REGULAR_EXPRESSION`, `PASS_REGULAR_EXPRESSION`,
  `FAIL_REGULAR_EXPRESSION`, or any unknown CTest property.
- No duplicate JSON object keys, unbounded/missing/symlinked JSON
  inventory, unrecognized test names or unreviewed command arguments.

The existing source Git-blob checker runs separately before compilation;
the JUnit verifier still requires all five cases to report `status=run`
after CTest execution. This added gate makes command/property substitution
fail closed before guests start, rather than misclassifying a negative CTest
as positive native parity.

## Evidence and limitations

**Locally executed source-free checks (2026-11-04):** the byte-identical
GitHub blobs `20f288f6a62c759cc4bee75d695aa3ed6f8ad66a`
(policy) and `b88be3b89454e46dde749ff5b2bdaf3c071c85db`
(adversarial cases) passed **25/25** Python tests. A separate real CMake
3.31/CTest 3.31 JSON-v1 five-test inventory passed the policy verifier,
with compiler/fixture files represented by inert local stand-ins only.
A real CTest `WILL_FAIL TRUE` experiment produced a *Passed* test from
an intentionally failing command, reproducing the bug.

These results prove **host-side policy behavior only**, not LLVM compiler
compilation, IR correctness, Clang linking, or AttoLang guest execution.
The LLVM source-owned private Actions jobs have consistently ended with
`runner_id=null` and `steps=null`; source-owned runner admission and
independent review are still required. No private Git source was mirrored
across organizations. Keep this PR draft pending exact-head stepful CI.
