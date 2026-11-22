#!/usr/bin/env bash
# Native AOT HTTP stdlib candidate: executable guest, not host-only proof.
set -euo pipefail
cd "$(dirname "$0")/.."
if [[ ! -x target/release/atto-llvm ]]; then
  cargo build --locked --release --bin atto-llvm
fi
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cat stdlib/http/message_policy.atto tests/http_stdlib/message_policy_canary.atto > "$work/http-guest.atto"
target/release/atto-llvm "$work/http-guest.atto" -o "$work/http-guest.exe"
file "$work/http-guest.exe"
set +e
timeout 15s "$work/http-guest.exe"
status=$?
set -e
if [[ "$status" != 42 ]]; then
  echo "FAIL: native HTTP guest exited $status, expected 42" >&2
  exit 1
fi
echo "PASS: native HTTP .atto policy guest, 34 cases, exit 42"
