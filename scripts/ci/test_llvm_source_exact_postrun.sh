#!/usr/bin/env bash
# Source-free, offline mock of post-run exact-PR-head authorization.
set -euo pipefail
workflow="${1:?usage: test_llvm_source_exact_postrun.sh WORKFLOW_FILE}"
[[ -f "$workflow" ]] || { echo 'missing workflow' >&2; exit 2; }
# Make deletion, echo-only replacement, or incorrect suite guards for the
# real actor-ledger integration fail the source-free handoff regression.
extract_step() {
  local name="$1"
  awk -v wanted="$name" '
    $0 == "      - name: " wanted { found=1; next }
    found && /^      - name: / { exit }
    found { print }
    END { if (!found) exit 12 }
  ' "$workflow"
}
grep -qxF '          - actor-ledger' "$workflow" || {
  echo 'missing actor-ledger dispatch choice' >&2; exit 1;
}
native_step="$(extract_step 'Run real ledger, scheduler STOP and pinned physical-heap native suite')"
grep -qxF "        if: inputs.suite == 'actor-ledger'" <<< "$native_step"
grep -qxF '          test -x scripts/ci/run_actor_ledger_native_exact_head.sh' <<< "$native_step"
grep -qxF '          bash scripts/ci/run_actor_ledger_native_exact_head.sh "$SOURCE_SHA"' <<< "$native_step"
for name in \
  'Verify preinstalled LLVM / Clang / CMake toolchain' \
  'Build actual Oreslang source compiler against LLVM' \
  'Compile and execute real Oreslang source actor fixtures'; do
  step="$(extract_step "$name")"
  grep -qxF "        if: inputs.suite == 'actor-llvm' || inputs.suite == 'actor-ledger'" <<< "$step" || {
    echo "actor-ledger missing actual compiler/guest dependency: $name" >&2; exit 1;
  }
done
guest_step="$(extract_step 'Execute real compiled Oreslang ledger guest fixtures')"
grep -qxF "        if: inputs.suite == 'actor-ledger'" <<< "$guest_step"
grep -qxF '          test -f tests/native_actor_compiled_guest_ledger.py' <<< "$guest_step"
grep -qxF '          PYTHONOPTIMIZE=0 python3 tests/native_actor_compiled_guest_ledger.py \' <<< "$guest_step"
san_step="$(extract_step 'Execute compiled ledger guests under Clang ASan and UBSan')"
grep -qxF "        if: inputs.suite == 'actor-ledger' && inputs.run_sanitizers" <<< "$san_step"
grep -qxF '          PYTHONOPTIMIZE=0 python3 tests/native_actor_compiled_guest_ledger.py \' <<< "$san_step"
printf 'SOURCEFREE_ACTOR_LEDGER_WORKFLOW_CONTRACT=PASS\n'

block="$(awk '
  /^      - name: Post-run attestation of current PR head and independent reviewer$/ { in_step=1; next }
  in_step && /^      - name: / { exit }
  in_step && /^        if: success\(\)$/ { gated=1 }
  in_step && /^        run: \|$/ { in_block=1; next }
  in_step && in_block && /^          / { print substr($0,11); next }
  in_step && in_block && /^$/ { print ""; next }
  END { if (!gated || !in_block) exit 12 }
' "$workflow")"
[[ "$block" == *POST_EXECUTION_EXACT_PR_HEAD_REVIEW=PASS* ]] || {
  echo 'missing post-run attestation marker' >&2; exit 2;
}
bash -n <<< "$block"
export GITHUB_REPOSITORY="ores-truffle-oreslang/oreslang-source.llvm"
export PR_NUMBER=1220
export SOURCE_SHA=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
export FAKE_HEAD="$SOURCE_SHA"
export FAKE_PR_SHA="$SOURCE_SHA"
export FAKE_PR_REPO="$GITHUB_REPOSITORY"
export FAKE_PR_STATE=open
export FAKE_PR_PRIVATE=true
export FAKE_AUTHOR_ID=7777
export FAKE_PROTECTED=true
export FAKE_REVIEW_STATE=APPROVED
export FAKE_REVIEW_SHA="$SOURCE_SHA"
export FAKE_REVIEWER_ID=11139560
git() {
  [[ "${1:-}" == rev-parse && "${2:-}" == HEAD && "$#" -eq 2 ]] || return 19
  printf '%s\n' "$FAKE_HEAD"
}
gh() {
  [[ "${1:-}" == api ]] || return 21
  case "${2:-}" in
    "repos/${GITHUB_REPOSITORY}/branches/main")
      printf '%s\n' "$FAKE_PROTECTED" ;;
    "repos/${GITHUB_REPOSITORY}/pulls/${PR_NUMBER}")
      printf '%s\t%s\t%s\t%s\t%s\n' "$FAKE_PR_SHA" "$FAKE_PR_REPO" "$FAKE_PR_STATE" "$FAKE_PR_PRIVATE" "$FAKE_AUTHOR_ID" ;;
    "repos/${GITHUB_REPOSITORY}/pulls/${PR_NUMBER}/reviews?per_page=100")
      printf '%s\t%s\t%s\n' "$FAKE_REVIEW_STATE" "$FAKE_REVIEW_SHA" "$FAKE_REVIEWER_ID" ;;
    *) return 22 ;;
  esac
}
export -f git gh
runs=0
if bash -c "$block" >/dev/null 2>&1; then
  ((runs+=1))
else
  echo 'valid exact-head independently approved result unexpectedly denied' >&2; exit 1
fi
expect_deny() {
  local label="$1" key="$2" value="$3" previous
  previous="${!key}"
  printf -v "$key" '%s' "$value"
  export "$key"
  if bash -c "$block" >/dev/null 2>&1; then
    echo "insecurely allowed case: $label" >&2
    return 1
  fi
  printf -v "$key" '%s' "$previous"
  export "$key"
  ((runs+=1))
}
expect_deny 'unprotected main' FAKE_PROTECTED false
expect_deny 'checkout SHA changed' FAKE_HEAD bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb
expect_deny 'PR head moved' FAKE_PR_SHA bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb
expect_deny 'foreign source repo' FAKE_PR_REPO foreign/repository
expect_deny 'PR closed' FAKE_PR_STATE closed
expect_deny 'PR made public' FAKE_PR_PRIVATE false
expect_deny 'review dismissed' FAKE_REVIEW_STATE DISMISSED
expect_deny 'review stale' FAKE_REVIEW_SHA bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb
expect_deny 'unauthorized reviewer' FAKE_REVIEWER_ID 11139561
expect_deny 'reviewer is author' FAKE_AUTHOR_ID 11139560
expect_deny 'invalid author identity' FAKE_AUTHOR_ID invalid
printf 'POSTRUN_MOCK_TESTS=%d PASS (1 allowed, 11 denied; no GitHub API calls)\n' "$runs"
