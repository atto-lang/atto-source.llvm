#!/usr/bin/env bash
# Offline source-free test: gh is shadowed by a temporary mock executable.
set -euo pipefail
helper="${1:?pass exact helper path}"
[[ -f "$helper" ]] || exit 2
original_path="$PATH"
temp="$(mktemp -d)"
trap 'rm -rf "$temp"' EXIT
mkdir -m 700 "$temp/secure"
mkdir "$temp/bin"
cat > "$temp/bin/gh" <<'GH'
#!/usr/bin/env bash
set -euo pipefail
if [[ "$1" == auth && "$2" == status ]]; then exit 0; fi
[[ "$1" == api ]] || exit 7
shift
if [[ "$1" == --method ]]; then
  request="$(cat)"
  jq -e --arg label "$EXPECTED_LABEL" '.runner_group_id==7 and (.labels|index($label))!=null' <<< "$request" >/dev/null
  [[ "${MOCK_SCENARIO:-}" != post_failure ]] || exit 8
  printf '{"encoded_jit_config":"%064d"}\n' 987
  exit 0
fi
case "$1" in
  "repos/$EXPECTED_REPO")
    jq -n --arg repo "$EXPECTED_REPO" '{id:123,full_name:$repo,private:true,archived:false,disabled:false,fork:false}' ;;
  "repos/$EXPECTED_REPO/branches/main")
    if [[ "${MOCK_SCENARIO:-}" == unprotected ]]; then echo '{"name":"main","protected":false}'; else echo '{"name":"main","protected":true}'; fi ;;
  "repos/$EXPECTED_REPO/branches/main/protection")
    good_protection='{"required_pull_request_reviews":{"require_code_owner_reviews":true,"dismiss_stale_reviews":true,"required_approving_review_count":1,"bypass_pull_request_allowances":{"users":[],"teams":[],"apps":[]}},"enforce_admins":{"enabled":true},"allow_force_pushes":{"enabled":false},"allow_deletions":{"enabled":false}}'
    case "${MOCK_SCENARIO:-}" in
      weak_review) jq -c '.required_pull_request_reviews.require_code_owner_reviews=false' <<< "$good_protection" ;;
      admin_bypass) jq -c '.required_pull_request_reviews.bypass_pull_request_allowances.users=[{"login":"admin"}]' <<< "$good_protection" ;;
      stale_review) jq -c '.required_pull_request_reviews.dismiss_stale_reviews=false' <<< "$good_protection" ;;
      no_admin_enforcement) jq -c '.enforce_admins.enabled=false' <<< "$good_protection" ;;
      force_push_allowed) jq -c '.allow_force_pushes.enabled=true' <<< "$good_protection" ;;
      delete_allowed) jq -c '.allow_deletions.enabled=true' <<< "$good_protection" ;;
      app_bypass) jq -c '.required_pull_request_reviews.bypass_pull_request_allowances.apps=[{"slug":"bypass-app"}]' <<< "$good_protection" ;;
      *) printf '%s\n' "$good_protection" ;;
    esac ;;
  "repos/$EXPECTED_REPO/contents/scripts/ci/prepare_source_ephemeral_jit.sh?ref=main")
    if [[ "${MOCK_SCENARIO:-}" == stale_helper ]]; then
      echo "0000000000000000000000000000000000000000"
    else
      git hash-object --no-filters -- "$EXPECTED_HELPER_PATH"
    fi ;;
  "repos/$EXPECTED_REPO/contents/.github/workflows/$EXPECTED_WORKFLOW?ref=main")
    jq -n --arg path ".github/workflows/$EXPECTED_WORKFLOW" '{path:$path,type:"file"}' ;;
  orgs/ores-truffle-oreslang/actions/runner-groups/7)
    if [[ "${MOCK_SCENARIO:-}" == overbroad_group ]]; then
      echo '{"id":7,"visibility":"all","allows_public_repositories":true,"restricted_to_workflows":false,"selected_workflows":[]}'
    else
      jq -n --arg workflow "$EXPECTED_REPO/.github/workflows/$EXPECTED_WORKFLOW@refs/heads/main" \
        '{id:7,visibility:"selected",allows_public_repositories:false,restricted_to_workflows:true,selected_workflows:[$workflow]}'
    fi ;;
  orgs/ores-truffle-oreslang/actions/runner-groups/7/repositories\?per_page=100)
    if [[ "${MOCK_SCENARIO:-}" == extra_repository ]]; then
      echo '{"total_count":2,"repositories":[{"id":123},{"id":124}]}'
    else
      jq -n --arg repo "$EXPECTED_REPO" '{total_count:1,repositories:[{id:123,full_name:$repo}]}'
    fi ;;
  *) echo "Unexpected mock GitHub API" >&2; exit 9;;
esac
GH
chmod +x "$temp/bin/gh"
EXPECTED_REPO="ores-truffle-oreslang/oreslang-source.llvm"
EXPECTED_WORKFLOW="llvm-source-exact-ephemeral.yml"
EXPECTED_LABEL="oreslang-llvm-ephemeral-v1"
export PATH="$temp/bin:$original_path"
EXPECTED_HELPER_PATH="$helper"
export EXPECTED_REPO EXPECTED_WORKFLOW EXPECTED_LABEL EXPECTED_HELPER_PATH
for scenario in unprotected weak_review admin_bypass stale_review no_admin_enforcement force_push_allowed delete_allowed app_bypass stale_helper overbroad_group extra_repository post_failure; do
  if MOCK_SCENARIO="$scenario" bash "$helper" 7 "$temp/secure/$scenario.b64" >"$temp/log" 2>&1; then
    echo "UNSAFE_JIT_ADMISSION=$scenario" >&2; exit 1
  fi
  [[ ! -e "$temp/secure/$scenario.b64" ]] || exit 1
  echo "PASS: deny $scenario"
done
if GITHUB_ACTIONS=true bash "$helper" 7 "$temp/secure/inside-actions.b64" >"$temp/log" 2>&1; then exit 1; fi
[[ ! -e "$temp/secure/inside-actions.b64" ]] || exit 1
echo 'PASS: deny provisioning inside GitHub Actions'
MOCK_SCENARIO=success bash "$helper" 7 "$temp/secure/valid.b64" >"$temp/log"
[[ -s "$temp/secure/valid.b64" && "$(stat -c %a "$temp/secure/valid.b64")" == 600 ]] || exit 1
! grep -q '0000000000000000' "$temp/log" || exit 1
if MOCK_SCENARIO=success bash "$helper" 7 "$temp/secure/valid.b64" >"$temp/log" 2>&1; then exit 1; fi
echo 'PASS: one-time secure config and no overwrite'
# The helper's pre-JIT integrity hash must NEVER execute Git clean filters.
# Mock API's expected SHA is deliberately computed with --no-filters too.
mkdir -p "$temp/filter-repo"
git -C "$temp/filter-repo" init -q
cp "$helper" "$temp/filter-repo/prepare_source_ephemeral_jit.sh"
printf '%s\n' 'prepare_source_ephemeral_jit.sh filter=tripwire' > "$temp/filter-repo/.gitattributes"
git -C "$temp/filter-repo" config filter.tripwire.clean "sh -c 'touch $temp/filter-ran; cat'"
old_expected_helper="$EXPECTED_HELPER_PATH"
EXPECTED_HELPER_PATH="$temp/filter-repo/prepare_source_ephemeral_jit.sh"
export EXPECTED_HELPER_PATH
(
  cd "$temp/filter-repo"
  MOCK_SCENARIO=success bash "$EXPECTED_HELPER_PATH" 7 "$temp/secure/filter-safe.b64" >"$temp/filter-log" 2>&1
)
[[ -s "$temp/secure/filter-safe.b64" && ! -e "$temp/filter-ran" ]] || {
  echo 'UNSAFE_JIT_HASH_FILTER_EXECUTED_OR_CREDENTIAL_MISSING' >&2
  exit 1
}
# Positive control ensures the adversarial filter really is executable.
git -C "$temp/filter-repo" hash-object -- "$EXPECTED_HELPER_PATH" >/dev/null
[[ -e "$temp/filter-ran" ]] || { echo 'inert malicious Git filter fixture' >&2; exit 1; }
EXPECTED_HELPER_PATH="$old_expected_helper"
export EXPECTED_HELPER_PATH
echo 'PASS: admin helper integrity hash ignores malicious clean filter'
echo 'SOURCEFREE_MOCK_JIT_ADMISSION=PASS'
