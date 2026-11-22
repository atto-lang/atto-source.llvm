# AttoLang llvm compiler contributor instructions

Follow [root AGENTS.md](../AGENTS.md) and [AOT/zero-GC controlled-dynamism checklist](../docs/AOT_ZERO_GC_DYNAMISM.md). Sealed guest AOT rejects runtime eval/class/method creation, unrestricted reflection, unknown proxies and interpreter/JIT fallback. Statically synthesize proxies/dispatch metadata and guarantee explicit ownership/drop across actor/task/closure suspension. Do not confuse host AOT or managed-host memory with zero-GC guest or process proof. Record positive + negative `.atto` tests and exact-head stepful guest execution; zero-step jobs are NOT_EXECUTED. Preserve Rust-only tooling and existing source-first/security constraints.

## Test and tooling policy

Write new test drivers, policy scanners, and repository automation in **Rust**, not Python.
Do not add new `.py` / `.pyi` files, Python invocations, or Python-based CI steps.
Keep existing legacy Python guest coverage intact until its Rust replacement executes
the same compiler-backed cases. Track transitional policy separately in issue #1617
and draft #1564; neither draft status nor a scanner-only check certifies migration.
Failures to read tracked execution inputs must fail closed, not appear as passing checks.

