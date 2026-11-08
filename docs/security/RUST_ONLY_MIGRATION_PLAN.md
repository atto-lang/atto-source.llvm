# Rust-only migration: explicit legacy freeze, not certification

**Status: DRAFT / TRANSITIONAL.  Full Rust-only migration is NOT_COMPLETE.**
This branch does **not** approve executing Python for new features or remove the
strict zero-Python target. The source repository is still dependent on legacy
Python tooling and CI. A full prohibition cannot truthfully pass against the
existing tree without breaking the conformance suites.

## Frozen source baseline

- Exact baseline commit: `443d2a1e150557f158fc5cfb457bde204f106dd2`.
- That commit contains **112 tracked .py/.pyi/.pyw/.ipynb paths** in its Git
  tree, plus other legacy tooling references. This is an observed lower bound,
  not a claim that only 112 Python-related dependencies remain.
- The baseline SHA is hardcoded in the Rust migration checker and may only
  change through independent CODEOWNER review. The checker verifies Git
  ancestry and a clean index/worktree before evaluating changes from baseline.
- New or changed Python artifacts are rejected. Added execution references
  in changed workflow, shell, Rust, JS, CMake and documentation surfaces are
  rejected even if the file previously contained older Python use.
- Deletions of Python files or removal of Python command lines reduce debt.
  No source files, workflows, permissions or conformance behaviors are dropped.

This **non-regression gate intentionally cannot assert zero Python**. It avoids
the initial contradiction where both newly introduced `no-python` workflows
always fail against their own source checkout. The strict Rust scanner
`tools/check_no_python.rs` remains available and its unit tests still run.
The tracked-source zero-Python check remains a separate *failing* certification
gate until existing Python functionality is migrated.

## Migration and strict-release sequence

1. Inventory every tracked Python source file, notebook, environment/lock file,
   workflow invocation, and docs/build dependency. Assign a Rust replacement
   plus a source-visible test/corpus for each item. Keep the inventory tied to
   exact Git blobs and an independently reviewed ledger.
2. Replace each Python harness with its Rust equivalent (including native
   conformance, semantic parser negatives and reproducible test receipts).
   Run old/new suites side by side using an *explicitly authorized* source
   runner until behavior and failure modes are equivalent; only then delete
   the old path.
3. Require the migration gate to reject any new/modified Python artifact or
   newly introduced execution line. A draft PR changing a legacy Python
   script must either migrate that functionality or remain blocked.
4. When zero Python artifacts, interpreter setup, execution references and
   documented Python dependencies remain, activate strict zero-Python policy
   under a separate reviewed switch. Only that all-files scan may issue a
   **RUST_ONLY_CERTIFIED** statement.

The migration checker uses Git file changes and strict Rust policy logic. It
does not rely on Python, code generation from external sources, or any private
CI carrier. No cross-organization private source mirroring is authorized here.

## Current AttoLang f64 gate

LLVM draft PR #1517 contains changes to legacy Python files and must not be
represented as compliant with this transitional policy until their replacement
or independent review. Its real f64 native tests, linked Wasm execution and
Java parity remain NOT_EXECUTED / NOT_CERTIFIED without a source-owned runner.
