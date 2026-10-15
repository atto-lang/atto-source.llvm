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

## Additional audit controls

The current smoke runner reads compiler output incrementally with a 32 KiB cap per stream, fails closed on truncation and timeout, and uses a dedicated POSIX process group to terminate a stalled launcher together with its descendants. Its admission unit tests cover output floods, child-process timeout, missing executables and invalid diagnostics. These controls do not create a security sandbox or disallow outbound network access; they require a trusted isolated CI runner.

The shared evidence adapter checks that compiler, fixture, linker and guest execution argv fields agree; LLVM one-argument executable launches are valid. These checks protect report *consistency* but cannot prove source-to-binary provenance or prevent a forged report.

## Source-identity audit

The smoke runner now independently reads the checked-out Git HEAD with git rev-parse --verify HEAD and refuses a caller-supplied --source-sha that differs from the actual checkout. It records the independently read hash as actual_git_checkout_sha; shared conformance evidence validation rejects missing or mismatched values. Regression tests exercise a real temporary Git repo and a deliberately forged source SHA that must fail before compilation.

For pull_request workflows, GITHUB_SHA may be a synthetic merge commit: treat it as the checked-out tree under test, not necessarily the PR branch head. SHA matching here still does not attest that the compiler artifact was reproducibly built from those sources.

The shared Actions diagnostic counts steps only when execution is evidenced; skipped, queued or missing step entries do not imply executed tests. A green diagnostic unit test is not a compiler conformance result.

## IR and native executable fingerprints

For the LLVM positive fixture, the producer now hashes the exact UTF-8 LLVM IR bytes written to the linker input and the generated native executable both before and after guest execution. The shared adapter requires the IR digest to match recorded compiler stdout and the two executable digests to match, rejecting omitted or inconsistent fields. If Clang reports success but produces no regular executable, the producer fails closed.

These checks detect unintended modification during an observed run but do **not** authenticate self-reported JSON, link the source tree cryptographically to compiler binaries, or prove the absence of a time-of-check/time-of-use race. Java continues to report only the launcher class digest rather than an entire Maven/JDK dependency closure. Public promotion still requires independent CI artifact/run verification.
