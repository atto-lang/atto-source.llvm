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
can be intentionally invalid. Use `--json <report-file>` to capture inventory.
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
