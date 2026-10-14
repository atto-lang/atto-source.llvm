# AI code generation: real compiler smoke evidence

This private repository's **LLVM conformance contract** workflow now includes a deliberately narrow smoke gate against the actual LLVM native compiler build.

## Test cases

- Positive: `pub fnc main(): int { return 20 + 22; }` must be accepted by the LLVM native compiler, and the resulting guest/native program must exit with status **42**.
- Negative: `pub fnc main(): int { return nonexistent_symbol; }` must be rejected with an actual compiler diagnostic mentioning the missing identifier.
- No mocks, fake checker, or "successful parse" is accepted as runtime verification.

The source text and exact artifact identity are SHA-256 hashed, and the check records argv, exit status, stdout, stderr, duration, backend, checkout SHA, and CI run ID in `build/ai-conformance-llvm.json`. This artifact is uploaded in the private CI job and may not be disclosed publicly without security review. The compiler executable/artifact fingerprint and the process invoker fingerprint are explicitly separate; Java hashes the launcher's compiled `OresMain.class`, while LLVM hashes its native `oreslang-llvmc` executable.

## Run locally after building

The lightweight admission tests can be run without a compiler:

```sh
python3 scripts/test_ai_conformance_smoke.py
```

The actual compiler smoke is part of [.github/workflows/llvm-conformance.yml](.github/workflows/llvm-conformance.yml) and should be run only after that workflow's existing compiler build/dependency setup has succeeded. **Runner unit tests are not compiler tests.**

## Known evidence limitations

- The report's `source_commit_declared` is checked against GitHub's checkout SHA when available, but the report does not independently attest that the binary was reproducibly built from that source. It records `source_commit_binary_link_attested: false`.
- For Java, the launcher class SHA does not capture all transitive classes and Maven dependencies. Full reproducible build provenance remains a separate milestone.
- For LLVM, successful LLVM IR generation + Clang linking + `main()` returning 42 demonstrates this fixture only; it says nothing about actors, async, ownership, or portable ABI correctness.
- `subprocess` uses bounded-duration calls and bounded in-memory output captures. It does **not** guarantee an OS security sandbox or network/filesystem isolation; the workflow must run only trusted fixtures on its private runner.
- The shared [platform-conformance repository](https://github.com/ores-truffle-oreslang/oreslang-platform-conformance) offers a report *consistency* adapter. A JSON report cannot authenticate itself. Only independently inspected successful real GitHub Actions runs can support later human-reviewed feature promotion.
- Do not copy the private compiler source to public CI repos or alternate organizations simply to obtain GitHub Actions minutes.

This smoke does not change the public machine-readable feature matrix, which still reports **no verified examples** until real evidence is available and reviewed.
