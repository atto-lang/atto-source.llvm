#!/usr/bin/env bash
# Validate the exact receipt set, not just aggregate counts.
set -euo pipefail
[[ $# == 2 ]] || exit 64
head=$1
log=$2
[[ "$head" =~ ^[a-f0-9]{40}$ && -f "$log" && ! -L "$log" ]] || exit 65
declare -A seen=()
guest_re='^TASK_I64_ATTO_GUEST_PASS\|head=([a-f0-9]{40})\|(O0|O2)\|exit=42\|compiler_sha256=([a-f0-9]{64})\|fixture_sha256=([a-f0-9]{64})\|binary_sha256=([a-f0-9]{64})$'
negative_re='^TASK_I64_ATTO_NEGATIVE_PASS\|head=([a-f0-9]{40})\|case=(missing-await|double-await|producer-call|second-task|actor-task-no-pool)\|no_artifact=yes$'
compiler=''
fixture=''
while IFS= read -r line || [[ -n "$line" ]]; do
  case "$line" in
    TASK_I64_ATTO_GUEST_PASS*)
      [[ "$line" =~ $guest_re ]] || exit 65
      [[ "${BASH_REMATCH[1]}" == "$head" ]] || exit 65
      key=${BASH_REMATCH[2]}
      if [[ -z "$compiler" ]]; then
        compiler=${BASH_REMATCH[3]}
        fixture=${BASH_REMATCH[4]}
      fi
      [[ "$compiler" == "${BASH_REMATCH[3]}" && "$fixture" == "${BASH_REMATCH[4]}" ]] || exit 65
      ;;
    TASK_I64_ATTO_NEGATIVE_PASS*)
      [[ "$line" =~ $negative_re ]] || exit 65
      [[ "${BASH_REMATCH[1]}" == "$head" ]] || exit 65
      key=${BASH_REMATCH[2]}
      ;;
    *) continue ;;
  esac
  [[ ! -v 'seen[$key]' ]] || exit 65
  seen[$key]=1
done < "$log"
for key in O0 O2 missing-await double-await producer-call second-task actor-task-no-pool; do
  [[ -v 'seen[$key]' ]] || exit 65
done
[[ "${#seen[@]}" == 7 ]] || exit 65
echo TASK_AOT_EXACT_RECEIPT_SET_PASS
