# Oreslang source formatting: LLVM integration status

The canonical `.ores` formatter is the Rust implementation at
[`ores-truffle-oreslang/oreslang-format`](https://github.com/ores-truffle-oreslang/oreslang-format).
Do not fork its formatting rules into C++.

## Non-mutating corpus audit

Install a reviewed `oresfmt` binary, then run from the LLVM repository root:

```sh
python3 -m unittest discover -s scripts -p 'test_oresfmt_corpus_audit.py'
python3 scripts/audit-oresfmt-corpus.py --formatter oresfmt tests/fixtures contracts/v1/fixtures
# Strict is for reviewed / intentionally migrated valid subsets only:
python3 scripts/audit-oresfmt-corpus.py --formatter oresfmt --strict tests/fixtures
```

The audit calls `oresfmt --stdout <file>` and byte-compares the result.
It never edits inputs, never follows symlinks, and reports formatter errors
separately from files that would change. Fixtures with `reject` suffixes
can be intentionally invalid. Use `--json <new-report-file>` to capture inventory.
JSON reports use exclusive creation: existing files, `.ores` sources,
symlinks, and hardlink aliases cannot be overwritten.
`--strict` is deliberately not a default CI check.

## Open compiler parity work

As of this integration, `src/main/cpp/oreslang/frontend/Parser.cpp`
does not parse Java's `import fnc/class/module/... from "...";` syntax.
This is **not** an assertion that such input executes on the LLVM backend.
Before enabling an import formatter gate, LLVM must implement compatible
import ASTs, path resolution, type/name binding, linked-module initialization
(including cycles and explicit `init()` hooks), and cross-backend tests.

The Java import graph can execute initialization hooks for imports whose
symbols are lexically unused. The formatter therefore preserves such imports
unless the author explicitly reviewed one as `// oresfmt: pure-import`.
`// oresfmt: keep-import` takes precedence. LLVM must honor compatible
semantics before sharing Java's full import corpus.

The read-only corpus inventory establishes **formatting compatibility only**.
It does not prove LLVM parser parity, native execution, or runtime semantics.

## Native `--analyze` boundary (experimental)

`oreslang-llvmc --analyze file.atto` (also legacy `.ores`) invokes
the **real native source loader/parser and LLVM source validation** but
does not print LLVM IR or execute guest code. After successful native
validation it deliberately exits **2** with the diagnostic
`exception-effect analysis unsupported ... effects inconclusive`.
A source rejected by the native compiler returns its ordinary error (1);
invalid or conflicting CLI flags return 2.

This is not a `throws/raises/panics` inference implementation. Current
`FunctionEffects` bits and native real-time checks do not resolve
typed Error ancestry, transitive exceptional effect sets, direct
defer/recover, actor panic CONTROL or dynamic unresolved calls.
A valid native parse and IR lowering **must not** be reported as an
effect-clean result. The only advisory effects checker remains
`atto test --effects --file file.atto` (Rust source hints).

The test `native_exception_effect_analyze_inconclusive_cli` runs the
compiled compiler on a valid `.atto`, a legacy `.ores`, invalid source
and incompatible option combinations. It also checks no source or
output artifacts are written. This is real compiler **admission evidence**
once the test has executed on a verified build, not guest exception
semantics or cross-backend parity. The formatter subprocess wrapper
requires an explicitly selected `ORESLANG_BIN`; pointed at this LLVM
compiler, its exit 2 remains 2.

Promotion to exit 0 requires the shared exceptional-callable-effects
contract: typed effect parser/AST and ABI, Error-only bounds, correct
call graph closure including unknown dispatch, recover subtraction,
actor panic bypass and supervisor CONTROL, plus executed Java/LLVM
source fixtures and exact-head CI. Do not change the 2 to 0 for
unimplemented cases.

## Compiler-AST observation slice (non-certifying)

The native `--analyze` mode additionally prints a bounded, human-readable
observation line to stderr, including `functions`, `call_edges`,
`unresolved_edges`, `throw_observed`, `raise_observed`,
`panic_observed`, `graph_unknown`, and `semantic_proof=0`. It uses the existing compiler-owned
`RealtimeAnalysis.hpp` direct-facts walker, then propagates those
observations through resolved function name/arity calls with a monotone
fixed-point algorithm. Recursive cycles cannot erase effects. Missing,
ambiguous, generic-unproven or unmodeled calls propagate an unresolved
marker; exceeded function or edge budgets are inconclusive.

These bits record **syntactically possible activity in admitted native AST**,
not typed escaping-effect sets. A locally caught throw may be reported, a
declared `using throw` cannot invent an observed throw, and a zero value
never proves a callable has no escaping effect. The scanned graph does not
fully model actors, class/constructor initialization, external imports,
dynamic dispatch, ordinary catch/recover paths, async continuation ownership,
or exception ABI. Exit 2 and the explicit `effects inconclusive` diagnostic
remain mandatory, even when the observed bits are zero.

The host-only CTest `native_exception_effect_observations_host_only`
exercises direct/transitive flags, missing-call propagation, recursion,
generic and duplicate identity, declared-effect non-certification, and
adversarial graph budgets. The real-compiler CTest
`native_exception_effect_analyze_inconclusive_cli` exercises native
`.atto` source including an already-supported caught throw and a
potentially failing arithmetic call, as well as unknown/unsupported grammar
and no-IR/no-mutation assertions. Neither test has yet yielded an
inspectable exact-head compiler execution receipt on the blocked source
organization; do not count them as PASS before a stepful authorized run.
