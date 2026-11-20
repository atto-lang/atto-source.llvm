#!/usr/bin/env bash
# Source-free, fail-closed admission regression; not guest execution.
set -euo pipefail
repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
gate="$repo/scripts/test-atto-nats-pretokenized-aot.sh"
test -f "$gate"
bash -n "$gate"
mkdir_temp="$(mktemp -d)"
trap 'rm -rf -- "$mkdir_temp"' EXIT
if env -u ATTO_NATS_APP_CHECKOUT -u ATTO_NATS_APP_EXPECTED_SHA \
    bash "$gate" >"$mkdir_temp/noargs.stdout" 2>"$mkdir_temp/noargs.stderr"; then
  echo 'MISSING_APP_CHECKOUT_WAS_ACCEPTED' >&2
  exit 1
fi
if env ATTO_NATS_APP_CHECKOUT="$mkdir_temp" \
  ATTO_NATS_APP_EXPECTED_SHA=not-a-sha \
  bash "$gate" >"$mkdir_temp/badsha.stdout" 2>"$mkdir_temp/badsha.stderr"; then
  echo 'INVALID_APP_SHA_WAS_ACCEPTED' >&2
  exit 1
fi
test ! -s "$mkdir_temp/noargs.stdout"
test ! -s "$mkdir_temp/badsha.stdout"
echo 'ATTO_NATS_LLVM_GATE_SOURCE_FREE_NEGATIVES=2'
echo 'ATTO_NATS_LLVM_REAL_AOT_EXECUTED=false'
