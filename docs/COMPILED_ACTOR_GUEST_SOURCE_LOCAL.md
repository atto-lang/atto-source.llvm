# Exact-head source-local compiled actor lifecycle execution

**Status: execution entrypoint committed, real compiler/guest execution not yet established.**

When GitHub Actions in `ores-truffle-oreslang` fails before runner admission, use a **trusted host within the authorized source repository boundary**, not an unapproved cross-org source mirror. This entrypoint is only for the existing wasm32 actual `.ores` actor lifecycle and full CTest. It **does not** satisfy the six-case actor isolation v1 contract.

## Prerequisites

- Independent reviewer supplies the full 40-character **exact PR source commit**. Never auto-derive the expected review identity from the currently checked-out `HEAD`.
- A **fresh, clean, private source checkout** at that commit; the checkout has no existing `build/`, altered tracked files, or untracked files.
- Trusted local Git, Bash, Python 3, Node, CMake, Clang with wasm32 link support, and an LLVM installation usable by this project's CMake.
- An execution environment governed by the source organization's private-code access and approved log/artifact controls. This script does **not** grant source-export authorization.

From the repository root:

```sh
EXPECTED_HEAD='<independently-reviewed-40-hex-source-commit>' \
  bash tests/run_pinned_compiled_actor_lifecycle.sh
```

The run refuses missing or mismatched `EXPECTED_HEAD`, positional skip flags, modified/untracked source, symlinked or reused `build/`, and execution from a subdirectory. It creates a fresh build, runs receipt capture against the actual compiled `build/oreslang-llvmc`, runs the source-free regression suites, verifies the compiled guest CTest command and pinned source/negative fixtures, executes the real source-actor wasm32 guest test, reverifies the compiler, executes the full registered CTest suite, and reverifies the compiler again.

The full `ctest` execution writes a CTest-generated JUnit report into that same temporary directory. `tests/verify_ctest_execution.py` re-reads the actual CTest JSON inventory and requires **all** registered tests (minimum 250) to have unique `status="run"` JUnit records, with no missing/duplicate results, skipped/disabled/failure/error cases, or inconsistent suite counts. Its source-free mutation suite is `tests/test_verify_ctest_execution.py`. A CTest exit code of zero alone is **not** sufficient. The JUnit report and gate result are source-local evidence only; they do not authenticate a CI runner and do not prove all six actor isolation scenarios.

The receipt is held in a private temporary directory and removed on exit. The console output includes source commit, source tree and compiler digest; the external evidence collector must preserve authenticated logs with the exact command, process exit status, host/toolchain provenance, and receipt bindings. A manually pasted string containing `PASS` is not independent execution proof.

## Fail-closed negative admission tests

These checks require no real LLVM source or compiler and can run on a developer machine:

```sh
python3 -m unittest discover -s tests \
  -p test_run_pinned_compiled_actor_lifecycle.py -v
```

They execute on fresh **synthetic Git repositories**, confirming the local entrypoint refuses missing, short or wrong reviewed commit, dirty source, untracked files, existing build or symlink, subdirectory invocation, and skip flags. Successful negatives do not prove a positive compiler run.

## Required acceptance after running

The verified **compiled actor lifecycle** result requires observed source-local compiler and real `.ores` guest execution, nonzero test steps, the exact reviewed commit/root tree/compiler digest, CTest registration, and complete test outcomes. The separate **six-case isolation v1** contract still requires compiler-negative foreign-access rejection, scoped-state loans, B surviving A retirement, generation-stale handle denial, quiescent reclamation, and memory scrubbing before actual free. The source-local lifecycle pass alone must never promote `guest_runtime_pass`.

For any alternate CI carrier, retain the independent source-export allowlist, reviewer, exact snapshot, pinned workflow, and destination authorization gates. Available Actions minutes and a private destination do not establish consent to move source.
