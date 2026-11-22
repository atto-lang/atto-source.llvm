#!/usr/bin/env bash
# Positive: all 16 pinned tracked source blobs.
# Negative: any untracked or ignored parser/compiler shadow file.
set -euo pipefail
cd "$(dirname "$0")/.."
bash -n ci/assert_legacy_frontend_frozen.sh
bash ci/assert_legacy_frontend_frozen.sh
shadow="frontend/__untracked_compiler_shadow_$$.rs"
if [[ -e "$shadow" || -L "$shadow" ]]; then
  echo "Cannot run regression: shadow probe already exists" >&2
  exit 1
fi
cleanup() { rm -f -- "$shadow"; }
trap cleanup EXIT
printf 'pub fn malicious_override() {}\n' > "$shadow"
if bash ci/assert_legacy_frontend_frozen.sh > /dev/null 2>&1; then
  echo 'FROZEN_FRONTEND_SHADOW_ACCEPTED: unexpected untracked source admission' >&2
  exit 1
fi
cleanup
trap - EXIT
bash ci/assert_legacy_frontend_frozen.sh
echo 'PASS: pinned frontend and rejection of untracked compiler shadow'
