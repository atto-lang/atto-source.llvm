#!/usr/bin/env bash
# Real canonical AttoLang source -> LLVM IR -> Clang executable admission gate.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
[[ $# == 1 && -x "$1" ]] || { echo 'Usage: test: compiler path required' >&2; exit 64; }
compiler="$(realpath "$1")"
tmp="$(mktemp -d)"
trap 'rm -rf -- "$tmp"' EXIT
fixture="$root/contracts/atto-rest-native-v1/rest_status_scalar.atto"
negative="$root/contracts/atto-rest-native-v1/rest_http_actor_unimplemented.atto"
"$compiler" "$fixture" > "$tmp/guest.ll"
grep -Fq 'define ' "$tmp/guest.ll" || { echo 'LLVM emitted no executable IR' >&2; exit 1; }
for opt in 0 2; do
  clang "-O$opt" -x ir "$tmp/guest.ll" -o "$tmp/guest-$opt"
  set +e
  "$tmp/guest-$opt"
  observed=$?
  set -e
  [[ "$observed" == 42 ]] || { echo "FAIL: native LLVM -O$opt guest exited $observed, expected 42" >&2; exit 1; }
  echo "ATTO_NATIVE_AOT_SCALAR_PASS backend=llvm opt=O$opt exit=42"
done
if "$compiler" "$negative" > "$tmp/unsupported.ll" 2> "$tmp/rejection.log"; then
  echo 'FAIL: unsupported REST HTTP/actor source was incorrectly accepted by scalar compiler' >&2
  exit 1
fi
if grep -Fq 'define ' "$tmp/unsupported.ll"; then
  echo 'FAIL: rejected REST HTTP/actor source emitted executable IR' >&2
  exit 1
fi
echo 'HTTP_ACTOR_SOURCE_REJECTED: full REST guest NOT_EXECUTED'
