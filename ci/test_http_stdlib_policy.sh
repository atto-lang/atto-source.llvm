#!/usr/bin/env bash
# Exact-checked-out-source native Atto HTTP scalar guest; never a host emulation.
set -euo pipefail
cd "$(dirname "$0")/.."

# Root Cargo.toml has only path dependencies; generate its lockfile offline,
# then enforce the generated local package graph for both tests and release.
cargo generate-lockfile --offline
cargo test --locked --offline --release --bin atto-llvm
cargo build --locked --offline --release --bin atto-llvm

expected_cases=46
observed_cases=$(grep -Ec '^  if .* then return [0-9]+; fi$' tests/http_stdlib/message_policy_canary.atto)
if [[ "$observed_cases" != "$expected_cases" ]]; then
  echo "FAIL: authored guest cases $observed_cases, expected $expected_cases" >&2
  exit 1
fi

work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT
cat stdlib/http/message_policy.atto tests/http_stdlib/message_policy_canary.atto > "$work/http-guest.atto"
target/release/atto-llvm "$work/http-guest.atto" -o "$work/http-guest.exe"
test -s "$work/http-guest.exe" || { echo 'FAIL: no guest artifact' >&2; exit 1; }
file "$work/http-guest.exe"
set +e
timeout 15s "$work/http-guest.exe"
status=$?
set -e
if [[ "$status" -ne 42 ]]; then
  echo "FAIL: native HTTP scalar guest exited $status, expected 42" >&2
  exit 1
fi
echo "ATTO_LLVM_HTTP_POLICY_GUEST_PASS authored_cases=$observed_cases exit=42 source_scope=single-unit"
