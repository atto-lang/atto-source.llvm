#!/usr/bin/env bash
# Admin-only JIT config helper. Never invoke this script from a GitHub workflow.
set -euo pipefail
if [[ "${GITHUB_ACTIONS:-}" == "true" ]]; then echo "DENIED: admin provisioning inside Actions" >&2; exit 1; fi
if [[ "$#" -ne 2 || ! "$1" =~ ^[1-9][0-9]{0,10}$ ]]; then
  echo "Usage: prepare_source_ephemeral_jit.sh RUNNER_GROUP_ID /secure/dir/config.b64" >&2
  exit 2
fi
group_id="$1"
output="$2"
for tool in gh jq openssl stat mktemp; do command -v "$tool" >/dev/null; done
[[ "${GH_HOST:-github.com}" == "github.com" ]] || { echo "DENIED: unsupported GitHub host" >&2; exit 1; }
[[ "$output" == /* && ! -e "$output" && ! -L "$output" ]] || { echo "DENIED: output must be new absolute file" >&2; exit 1; }
parent="${output%/*}"
[[ -d "$parent" && ! -L "$parent" ]] || { echo "DENIED: output parent invalid" >&2; exit 1; }
[[ "$(stat -c %u "$parent")" == "$(id -u)" && "$(stat -c %a "$parent")" == "700" ]] || {
  echo "DENIED: configuration output directory must be owned by caller and mode 0700" >&2; exit 1;
}
backend="llvm"
repository="ores-truffle-oreslang/oreslang-source.llvm"
runner_label="oreslang-llvm-ephemeral-v1"
workflow_file="llvm-source-exact-ephemeral.yml"
workflow_ref="ores-truffle-oreslang/oreslang-source.llvm/.github/workflows/llvm-source-exact-ephemeral.yml@refs/heads/main"
org="ores-truffle-oreslang"
if ! gh auth status >/dev/null 2>&1; then echo "DENIED: GitHub admin CLI authentication missing" >&2; exit 1; fi

checking_repo="$(gh api "repos/${repository}")"
jq -e --arg repo "$repository" '.full_name==$repo and .private==true and .archived==false and .disabled==false and .fork==false' \
  <<< "$checking_repo" >/dev/null || { echo "DENIED: source repository identity/state" >&2; exit 1; }
repo_id="$(jq -er '.id | select(type=="number")' <<< "$checking_repo")"
branch="$(gh api "repos/${repository}/branches/main")"
jq -e '.name=="main" and .protected==true' <<< "$branch" >/dev/null || {
  echo "DENIED: protected source main is not enforced" >&2; exit 1;
}
# "protected:true" by itself is not an independent review gate.
# Only real enforced branch protection (not a weak rule that sets the badge) admits JIT.
protection="$(gh api "repos/${repository}/branches/main/protection")"
jq -e '
  (.required_pull_request_reviews | type=="object")
  and (.required_pull_request_reviews.require_code_owner_reviews==true)
  and (.required_pull_request_reviews.dismiss_stale_reviews==true)
  and (.required_pull_request_reviews.required_approving_review_count | type=="number")
  and (.required_pull_request_reviews.required_approving_review_count>=1)
  and (.enforce_admins.enabled==true)
  and (.allow_force_pushes.enabled==false)
  and (.allow_deletions.enabled==false)
  and ([.required_pull_request_reviews.bypass_pull_request_allowances?
       | (.users // []), (.teams // []), (.apps // [])] | all(.==[]))
' <<< "$protection" >/dev/null || {
  echo "DENIED: protected main does not enforce CODEOWNER/stale/admin/no-bypass" >&2; exit 1;
}
workflow_meta="$(gh api "repos/${repository}/contents/.github/workflows/${workflow_file}?ref=main")"
jq -e --arg path ".github/workflows/$workflow_file" '.path==$path and .type=="file"' \
  <<< "$workflow_meta" >/dev/null || { echo "DENIED: main workflow not present" >&2; exit 1; }

group="$(gh api "orgs/${org}/actions/runner-groups/${group_id}")"
jq -e --argjson id "$group_id" --arg workflow "$workflow_ref" '
  .id==$id and .visibility=="selected" and .allows_public_repositories==false
  and .restricted_to_workflows==true and .selected_workflows==[$workflow]
' <<< "$group" >/dev/null || { echo "DENIED: runner group permissions/workflow restrictions" >&2; exit 1; }
visible="$(gh api "orgs/${org}/actions/runner-groups/${group_id}/repositories?per_page=100")"
jq -e --arg repo "$repository" --argjson id "$repo_id" '
  .total_count==1 and (.repositories | type=="array" and length==1)
  and .repositories[0].full_name==$repo and .repositories[0].id==$id
' <<< "$visible" >/dev/null || { echo "DENIED: runner group exposes other repositories" >&2; exit 1; }

umask 077
tmp="$(mktemp "${parent}/.jit-config.XXXXXXXX")"
trap 'rm -f "$tmp"' EXIT
runner_name="oreslang-${backend}-jit-$(openssl rand -hex 10)"
jq -n --arg name "$runner_name" --arg label "$runner_label" --argjson gid "$group_id" '
  {name:$name,runner_group_id:$gid,labels:["self-hosted","linux","x64",$label],work_folder:"_work"}
' | gh api --method POST "orgs/${org}/actions/runners/generate-jitconfig" --input - \
  | jq -er '.encoded_jit_config | select(type=="string" and length>50)' > "$tmp"
[[ -s "$tmp" ]] || { echo "DENIED: empty JIT credential" >&2; exit 1; }
chmod 600 "$tmp"
ln "$tmp" "$output"  # atomic hard-link fails rather than overwriting an existing secret
rm -f "$tmp"
trap - EXIT
echo "JIT_CONFIG_PREPARED=YES"
echo "RUNNER_LABEL=${runner_label}"
echo "SOURCE_REPOSITORY=${repository}"
echo "ONE_TIME_CONFIG_PATH=${output}"
echo "Do not print the configuration. Transfer only to isolated clean runner VM and destroy after one job."
