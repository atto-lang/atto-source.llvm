# Inverted SoA LLVM PR #1796: private exact-tree CI review packet

**Status: REVIEW ONLY / DENY EXPORT.** This document does not authorize private-source copying, alter an allowlist, bypass publisher v3, or constitute security-owner approval.

## Frozen candidate identities (2026-11-14; verify again before any use)

- Private source: `ores-truffle-oreslang/oreslang-source.llvm` (numeric repo ID `1410897874`).
- Subject: [PR #1796](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/1796), head `511cb80eb6892796d82c6864f6ac1c8a046a4038`, root tree `481b955126090913948ca77749c9887661c7b8dc`.
- Candidate private carrier: `streempilot-test/browser-studio-e2e`, repository ID `1323599136`, observed base `0fd571f333009bb3365ae9932f2ee9fc464f5b45`.
- Carrier Actions health: [run 43643635234](https://github.com/streempilot-test/browser-studio-e2e/actions/runs/43643635234), actually executed the C++/Java version-check step. **Runner health is not a source-build receipt or proof of future billing budget.**
- Source `main`: `protected:false`; carrier `main`: `protected:false`. Source governance requires protected source and independently reviewed change control.
- Current source `.github/private-ci-approved-repositories.txt` names this destination, **but this alone is not sufficient**. Existing `.github/private-ci-carrier-contracts.json` pins only historical `llvm-pr787` for that carrier. Current carrier `main` does **not** contain the historically pinned carrier workflow or test plan. No PR-1796-specific approval/plan is effective.

## Reviewer actions — mandatory before private tree export

1. Independently enforce and verify protected source `main`, CODEOWNER reviews, required status checks, no bypass and stale-review dismissal. Verify effective protection with authoritative GitHub metadata; missing/403 enforcement evidence must fail closed.
2. Select a dedicated private carrier or independently approve the current carrier for **this exact PR**; enforce destination integrity/protection or an independently attested equivalent. Reconfirm numeric repo ID, visibility and permissions.
3. Create and independently review a minimal carrier workflow with pinned action digests, read-only `GITHUB_TOKEN`, no cross-org checkout, secrets, third-party upload, PR-supplied scripts with elevated access, or network exfiltration. Pin its immutable Git blob SHA and approved carrier-base commit.
4. Review an exact immutable `ci/test-plans/llvm-pr1796.json` and the source-owned v3 publisher. The PR ID, commit and entire root tree must match. Explicitly test tree-completeness, symlink/submodule semantics, SHA equality for every blob, atomic ref rollback, ephemeral branch cleanup, no force-on-unknown, and denial before source reads/writes on any policy mismatch.
5. Only after successful independent approval merge the protected-source allowlist/contract update using a separate reviewed PR. Do not make a private Git object in an unapproved organization on the strength of this packet.

## Test plan after approval

Original source-tree commands, executed at frozen head (not source-free replacements):

```bash
set -euo pipefail
test "$(git rev-parse HEAD)" = "511cb80eb6892796d82c6864f6ac1c8a046a4038"
test "$(git rev-parse 'HEAD^{tree}')" = "481b955126090913948ca77749c9887661c7b8dc"
for compiler in g++ clang++; do
  "$compiler" -std=c++20 -pthread -Wall -Wextra -Wpedantic -Werror -O2 \
    -Isrc/main/cpp tests/dop_inverted_columns_test.cpp -o /tmp/dop-inverted
  /tmp/dop-inverted
  "$compiler" -std=c++20 -pthread -Wall -Wextra -Wpedantic -Werror -O1 -g \
    -fsanitize=address,undefined -fno-omit-frame-pointer -Isrc/main/cpp \
    tests/dop_inverted_columns_test.cpp -o /tmp/dop-inverted-asan
  ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
    UBSAN_OPTIONS=halt_on_error=1 /tmp/dop-inverted-asan
done
```

Add negative admission and actual `.atto` guest AOT runs separately; this native header test alone is not guest-AOT proof. Record executed steps, job logs, compiler versions, full tree SHA, sanitizer verdict, source head and exit codes. If a run has `steps:null`, record `NOT_EXECUTED`, never pass or fail the source.

## Current result

Source-owned [targeted LLVM workflow](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/pull/1796) has been committed to PR head. Its initial matrix run `43644855913` completed with **zero executed steps**. Full backend regressions remain `NOT_EXECUTED`. Track reviewer decision in [issue #1804](https://github.com/ores-truffle-oreslang/oreslang-source.llvm/issues/1804).
