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

# The mock tests belong to trusted main, not a PR-provided checkout. Also,
# native acceptance must run before CMake writes build-source-exact because the
# pinned native runner rejects any untracked/dirty source tree.
step_line() {
  local name="$1"
  awk -v wanted="$name" '$0 == "      - name: " wanted { print NR; exit }' "$workflow"
}
assert_before() {
  local left="$1" right="$2" left_n right_n
  left_n="$(step_line "$left")"
  right_n="$(step_line "$right")"
  [[ "$left_n" =~ ^[1-9][0-9]*$ && "$right_n" =~ ^[1-9][0-9]*$ &&
     "$left_n" -lt "$right_n" ]] || {
    echo "source-exact step order invalid: $left must precede $right" >&2
    exit 1
  }
}
trusted_step="$(extract_step 'Checkout trusted main for source-free workflow tests')"
grep -qxF '          ref: ${{ github.sha }}' <<< "$trusted_step"
grep -qxF '          persist-credentials: false' <<< "$trusted_step"
mock_step="$(extract_step 'Execute trusted source-free post-run authorization regression mocks')"
grep -qxF '          test "$(git rev-parse HEAD)" = "$GITHUB_SHA"' <<< "$mock_step"
grep -qxF '          bash scripts/ci/test_llvm_source_exact_postrun.sh .github/workflows/llvm-source-exact-ephemeral.yml' <<< "$mock_step"
grep -qxF '          python3 scripts/ci/test_verify_llvm_exact_checkout.py' <<< "$mock_step"
grep -qxF '          python3 scripts/ci/test_materialize_llvm_exact_checkout.py' <<< "$mock_step"
grep -qxF '          git cat-file blob HEAD:scripts/ci/materialize_llvm_exact_checkout.py > "$RUNNER_TEMP/materialize_llvm_exact_checkout.py"' <<< "$mock_step"
grep -Fq -- 'git hash-object --no-filters scripts/ci/materialize_llvm_exact_checkout.py' <<< "$mock_step"
grep -qxF '          git cat-file blob HEAD:scripts/ci/verify_llvm_exact_checkout.py > "$RUNNER_TEMP/verify_llvm_exact_checkout.py"' <<< "$mock_step"
grep -Fq -- 'git hash-object --no-filters scripts/ci/verify_llvm_exact_checkout.py' <<< "$mock_step"
exact_step="$(extract_step 'Require exact tree and preinstalled native toolchain')"
grep -qxF '          python3 "$RUNNER_TEMP/verify_llvm_exact_checkout.py" "$SOURCE_SHA"' <<< "$exact_step"
grep -qxF '          python3 "$RUNNER_TEMP/materialize_llvm_exact_checkout.py" "$SOURCE_SHA"' <<< "$exact_step"
if grep -Fq 'git diff --quiet' <<< "$exact_step"; then
  echo 'unsafe Git diff in exact-source preflight' >&2; exit 1
fi
pr_checkout="$(extract_step 'Fetch pinned PR objects into empty sparse checkout (never persist auth)')"
grep -qxF '          ref: ${{ inputs.source_sha }}' <<< "$pr_checkout"
grep -qxF '          persist-credentials: false' <<< "$pr_checkout"
grep -qxF '          filter: blob:limit=134217728' <<< "$pr_checkout"
grep -qxF '          sparse-checkout-cone-mode: false' <<< "$pr_checkout"
grep -qxF '          sparse-checkout: |' <<< "$pr_checkout"
grep -qxF '            /*' <<< "$pr_checkout"
grep -qxF '            !/*' <<< "$pr_checkout"
assert_before 'Verify open same-repo PR and exact immutable head before checkout' 'Checkout trusted main for source-free workflow tests'
assert_before 'Checkout trusted main for source-free workflow tests' 'Execute trusted source-free post-run authorization regression mocks'
assert_before 'Execute trusted source-free post-run authorization regression mocks' 'Fetch pinned PR objects into empty sparse checkout (never persist auth)'
assert_before 'Fetch pinned PR objects into empty sparse checkout (never persist auth)' 'Require exact tree and preinstalled native toolchain'
assert_before 'Require exact tree and preinstalled native toolchain' 'Run real ledger, scheduler STOP and pinned physical-heap native suite'
assert_before 'Run real ledger, scheduler STOP and pinned physical-heap native suite' 'Build actual Oreslang source compiler against LLVM'
printf 'SOURCEFREE_TRUSTED_CHECKOUT_AND_NATIVE_BEFORE_BUILD=PASS\n'

# CGL is a real executed native + ELF compatibility gate, not an echo receipt.
# The source-only handoff must fail review if future edits drop its negative
# preflight, named CTest targets, source-to-ELF exit-42 smoke, or ASan/UBSan.
grep -qxF '          - cgl-compat' "$workflow"
cgl_step="$(extract_step 'Execute exact-head CGL negative preflight, native compiler, CTest and guest ELF')"
cgl_san_step="$(extract_step 'Execute linked CGL native tests with Clang ASan and UBSan')"
cgl_require() {
  local needle="$1"
  grep -Fq -- "$needle" <<< "$cgl_step" || {
    echo "CGL suite lost required execution dependency: $needle" >&2
    exit 1
  }
}
for source_path in \
  tests/test_cgl_preflight.py \
  tests/fixtures/forward-call-main.ores \
  tests/native_cgl_host_admission_test.cpp \
  tests/native_cgl_dispatch_evidence_test.cpp \
  tests/native_cgl_carrier_slice_trace_test.cpp \
  tests/native_source_i64_executable.py \
  scripts/cgl-native-smoke.sh \
  scripts/cgl-preflight.py \
  scripts/compile-aot.sh; do
  cgl_require "test -f $source_path"
done
for native_target in \
  oreslang-llvmc \
  oreslang-native-cgl-host-admission-test \
  oreslang-native-cgl-dispatch-evidence-test \
  oreslang-native-cgl-carrier-slice-trace-test; do
  cgl_require "$native_target"
done
for ctest_name in \
  native_cgl_linux_host_admission \
  native_cgl_timer_to_user_start_evidence \
  native_cgl_carrier_slice_timestamp_provenance \
  native_source_i64_os_exit_subset; do
  cgl_require "$ctest_name"
done
cgl_require "PYTHONOPTIMIZE=0 python3 -I -B -m unittest discover -s tests -p test_cgl_preflight.py -v"
cgl_require 'ctest --test-dir build-cgl-source-exact --output-on-failure'
cgl_require '--no-tests=error -R "^${named_test}$"'
cgl_require 'bash scripts/cgl-native-smoke.sh "$PWD/build-cgl-source-exact"'
cgl_require 'CGL_SOURCE_TO_ELF_EXIT42_AND_CTEST_EXECUTED_FOR_SHA=$SOURCE_SHA'
grep -qxF "        if: inputs.suite == 'cgl-compat'" <<< "$cgl_step"
grep -qxF "        if: inputs.suite == 'cgl-compat' && inputs.run_sanitizers" <<< "$cgl_san_step"
for san_needle in \
  '-DCMAKE_CXX_COMPILER=clang++' \
  '-DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"' \
  '-DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"' \
  'ctest --test-dir build-cgl-asan-ubsan --output-on-failure' \
  '--no-tests=error -R "^${named_test}$"' \
  'CGL_CLANG_ASAN_UBSAN_LINKED_TESTS_EXECUTED_FOR_SHA=$SOURCE_SHA'; do
  grep -Fq -- "$san_needle" <<< "$cgl_san_step" || {
    echo "CGL sanitizer suite lost required execution dependency: $san_needle" >&2
    exit 1
  }
done
assert_before 'Require exact tree and preinstalled native toolchain' 'Execute exact-head CGL negative preflight, native compiler, CTest and guest ELF'
assert_before 'Execute exact-head CGL negative preflight, native compiler, CTest and guest ELF' 'Execute linked CGL native tests with Clang ASan and UBSan'
assert_before 'Execute linked CGL native tests with Clang ASan and UBSan' 'Post-run attestation of current PR head and independent reviewer'
printf 'SOURCEFREE_CGL_NATIVE_ELF_SANITIZER_WORKFLOW_CONTRACT=PASS\n'

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
