#!/usr/bin/env bash
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
verify="$root/verify-source-task-aot-receipts.sh"
tmp="$(mktemp -d)"
trap 'rm -rf -- "$tmp"' EXIT
head="$(printf '%040d' 1)"
digest="$(printf '%064d' 2)"
for opt in O0 O2; do
  printf 'TASK_I64_ATTO_GUEST_PASS|head=%s|%s|exit=42|compiler_sha256=%s|fixture_sha256=%s|binary_sha256=%s\n' "$head" "$opt" "$digest" "$digest" "$digest"
done > "$tmp/valid"
for name in missing-await double-await producer-call second-task actor-task-no-pool actor-task-nonsuspends; do
  printf 'TASK_I64_ATTO_NEGATIVE_PASS|head=%s|case=%s|no_artifact=yes\n' "$head" "$name"
done >> "$tmp/valid"
bash "$verify" "$head" "$tmp/valid"
deny() {
  if bash "$verify" "$head" "$1"; then echo "invalid receipt accepted: $1" >&2; exit 1; fi
}
sed '$d' "$tmp/valid" > "$tmp/missing"; deny "$tmp/missing"
sed 's/case=actor-task-no-pool/case=missing-await/' "$tmp/valid" > "$tmp/duplicate"; deny "$tmp/duplicate"
sed 's/|O2|/|O0|/' "$tmp/valid" > "$tmp/duplicate-opt"; deny "$tmp/duplicate-opt"
sed 's/no_artifact=yes/no_artifact=no/' "$tmp/valid" > "$tmp/artifact"; deny "$tmp/artifact"
sed "s/head=$head/head=$(printf '%040d' 9)/" "$tmp/valid" > "$tmp/head"; deny "$tmp/head"
sed '2s/compiler_sha256=0/compiler_sha256=1/' "$tmp/valid" > "$tmp/compiler"; deny "$tmp/compiler"
sed 's/exit=42/exit=0/' "$tmp/valid" > "$tmp/exit"; deny "$tmp/exit"
echo 'TASK_AOT_RECEIPT_VERIFIER_SELFTEST_PASS positive=1 negative=7'
