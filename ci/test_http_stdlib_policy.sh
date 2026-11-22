#!/usr/bin/env bash
# Native AOT HTTP stdlib candidate: executable guest, not host-only proof.
set -euo pipefail
cd "$(dirname "$0")/.."
# Never execute an unverified/stale compiler binary cached from a prior
# source head; rebuild exactly the checked-out source each CI invocation.
cargo build --locked --release --bin atto-llvm

# Gate the exact authored count so appending tests without updating the
# evidence expectation cannot produce an accidental green receipt.
expected_cases=46
observed_cases=$(grep -Ec '^  if .* then return [0-9]+; fi
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
echo "PASS: native HTTP .atto policy guest, $observed_cases cases, exit 42"
 tests/http_stdlib/message_policy_canary.atto)
if [[ "$observed_cases" != "$expected_cases" ]]; then
  echo "FAIL: authored guest cases $observed_cases, expected $expected_cases" >&2
  exit 1
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
