# Ownership-bang: source-local isolated Linux runner recovery

**Status: DRAFT / OFF BY DEFAULT / NOT_EXECUTED / NO RUNNER REGISTERED.** This source-local workflow proposal is neither admin approval nor private source export authorization.

Repository: `ores-truffle-oreslang/oreslang-source.llvm` (private). Target immutable CODE commit: `84c1bddf1807888c5f5904e18d14c71cb6b01d26`. The CI-only workflow PR has a **different** reviewed SHA.

## Required authorized administrator actions

1. Independently inspect source-org [CI admission #427](https://github.com/ores-truffle-oreslang/oreslang-source.net/issues/427): owner-visible Actions billing/budgets, spending stops, policy, hosted eligibility, runner allocation and failed job annotations. The available GitHub connector does not expose these administrative settings. Even a registered self-hosted runner may be blocked by organization-wide Actions policy.
2. Provision an **isolated single-use ephemeral Linux x64 host**. In this source repository's Settings → Actions → Runners, register it using GitHub's short-lived registration instructions, with standard self-hosted/linux/x64 labels and custom label `oreslang-bang-llvm-isolated`. Do not paste registration tokens, GitHub access tokens, secrets or machine credentials into the repo, workflow, chat or logs. Restrict runner group access to this repository; avoid privileged Docker, long-lived host credentials, and shared workloads.
3. Preinstall trusted dependencies before registration. Java: JDK/GraalVM and Maven. LLVM: Clang/LLVM development packages, CMake, C++, Python 3, and OpenSSL Crypto. Make sure the host can reach needed dependency registries without acquiring privileged credentials.
4. Independently review this CI-only PR's workflow file and the SOURCE code SHA `84c1bddf1807888c5f5904e18d14c71cb6b01d26` including dependencies, source files and test fixtures. A successful carrier canary elsewhere is not approval. A change to either reviewed commit invalidates authorization.
5. Set these **nonsecret source-repository Actions variables** after review:
   - `ORESLANG_BANG_RUNNER_ENABLED=enabled`
   - `ORESLANG_BANG_APPROVED_WORKFLOW_SHA=<current full 40-hex head of this CI-only PR>`
   - `ORESLANG_BANG_APPROVED_SOURCE_SHA=84c1bddf1807888c5f5904e18d14c71cb6b01d26`
   The workflow also pins the SOURCE SHA as a literal, so a variable change alone cannot retarget it.
6. Add label `ownership-bang-ci-approved` to the **CI-only PR**. This emits `pull_request:labeled`. The job starts only when the workflow PR is same-repository, based on main, and both admin-set SHA variables match the exact reviewed values. Label alone is not authorization.
7. Inspect the resulting job's **actual executed steps** and the verified artifact. After the run, remove the approval label, set the enable variable to `disabled`, retire the ephemeral host and revoke its short-lived runner credentials. An unrelated failed/skipped job must not be reported as a compiler test result.

## Exact-source commands and evidence

Source commit: `84c1bddf1807888c5f5904e18d14c71cb6b01d26` (this is distinct from the workflow PR head).

Expected immutable source **root Git tree**: `4203e26e774eae11086a989c6a512b7f19609f08` (read from the source repository Git commit-object API, not inferred from a historical PR). The workflow explicitly rejects a different checkout tree before any compiler step. Independent administrative approval is still required.

```bash
cmake -S . -B build -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --target oreslang-llvmc oreslang-native-ownership-bang-aliases-test --parallel 2
ctest --test-dir build --output-on-failure -R '^native_ownership_bang_'
```

Scope: LLVM compiled native guest execution, ownership checker, native O0/O2 cases and negative CTests; no cross-backend parity claim. The workflow checks out only the immutable source commit from the SAME repository using read-only `GITHUB_TOKEN` and `persist-credentials:false`, verifies Git HEAD and source tree, then executes real tests. It uploads a three-day evidence artifact **only on successful test completion**.

Required acceptance: source repository identity, reviewed CI workflow PR SHA, independently verified source commit/tree, run ID and attempt, assigned runner identity, nonempty completed test steps, positive JUnit/CTest counts, no skipped/disabled mandatory tests, and independently downloaded artifact ZIP SHA-256 and report hashes.

Java XML suites must contain >0 tests with zero failures, errors and skips; the Java guest suite validates six SHA-pinned Atto guest fixtures and exact stdout `42`. This is NOT Native Image AOT.
LLVM must build actual compiler/checker targets and run at least 20 ownership-bang CTests, including the required default, O0 and O2 guest variants and expected negative diagnostic cases. Host-only/sanitizer-runtime tests are not automatically IR instrumentation proof.

Source fix [LLVM #1903](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/1903) was source-locally rebased onto newer main, invalidating old-head run results; independently re-approve both the new source tree and current workflow PR commit.

**Zero-step, queued, cancelled, skipped, wrong-head, or missing-artifact runs are NOT_EXECUTED/UNCERTIFIED.** Do not infer compiler pass, merge readiness or release qualification from them.

## Security

The job has `contents:read`, pinned checkout and artifact actions, no workflow secrets, no token-bearing mirror, and no `pull_request_target` event. GitHub warns that self-hosted runners can be compromised by untrusted PR code; permit only deliberately reviewed SHAs on ephemeral hosts with no privileged infrastructure access. This avoids cross-org source export and leaves [independent governance #260](https://github.com/ores-truffle-oreslang/oreslang-source.net/issues/260) untouched.

Reference: [GitHub self-hosted runner security guidance](https://docs.github.com/en/actions/reference/security/secure-use), [source admission #427](https://github.com/ores-truffle-oreslang/oreslang-source.net/issues/427).
