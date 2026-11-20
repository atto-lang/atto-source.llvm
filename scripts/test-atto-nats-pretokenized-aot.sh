#!/usr/bin/env bash
# Backend-owned, exact-guest AttoNATS -> LLVM AOT admission.
# Requires two separately authorized EXISTING private checkout roots.
# It does NOT transfer source, clone across orgs, or substitute a host model.
set -euo pipefail
llvm="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
: "${ATTO_NATS_APP_CHECKOUT:?set ATTO_NATS_APP_CHECKOUT to an authorized existing private application checkout}"
: "${ATTO_NATS_APP_EXPECTED_SHA:?set exact 40-hex application commit}"
app="$(cd "$ATTO_NATS_APP_CHECKOUT" && pwd -P)"
[[ "$ATTO_NATS_APP_EXPECTED_SHA" =~ ^[a-f0-9]{40}$ ]] || {
  echo 'ATTO_NATS_EXPECTED_APP_SHA_INVALID' >&2; exit 64;
}
llvm_sha="$(git -C "$llvm" rev-parse HEAD)"
app_sha="$(git -C "$app" rev-parse HEAD)"
test "$app_sha" = "$ATTO_NATS_APP_EXPECTED_SHA" || {
  echo 'ATTO_NATS_APP_HEAD_MISMATCH' >&2; exit 65;
}
# Prevent unnoticed tracked source mutations in either backend. Generated
# compiler build outputs in ignored build/ dirs do not count as source.
if [[ -n "$(git -C "$llvm" status --porcelain --untracked-files=no)" ||
      -n "$(git -C "$app" status --porcelain --untracked-files=no)" ]]; then
  echo 'TRACKED_SOURCE_DIRTY' >&2
  exit 65
fi
test -x "$app/ci/run-native-llvm-token-match.sh" || {
  echo 'CANONICAL_ATTO_NATS_EXECUTION_GATE_MISSING' >&2; exit 69;
}
printf 'LLVM_SOURCE_HEAD=%s\n' "$llvm_sha"
printf 'ATTO_NATS_APP_HEAD=%s\n' "$app_sha"
ATTO_NATS_LLVM_CHECKOUT="$llvm" \
ATTO_NATS_LLVM_EXPECTED_SHA="$llvm_sha" \
bash "$app/ci/run-native-llvm-token-match.sh"
echo 'LLVM_ATTO_NATS_TOKEN_MATCH_REAL_AOT=PASS'
echo 'LLVM_FULL_NATS_BROKER_COMPATIBILITY=false'
echo 'LLVM_ACTOR_SEALED_STATE_EXECUTED=false'
echo 'LLVM_NETWORK_THROUGHPUT_MEASURED=false'
