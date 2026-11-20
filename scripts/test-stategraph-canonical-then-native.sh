#!/usr/bin/env bash
# Compiles real Oreslang native LLVM IR, Clang-links and executes a canonical
# if/then guest; negative assignment guard must reject without any emitted IR.
# This is a small CTest regression, NOT full generic StateGraph parity.
set -euo pipefail
[[ $# -eq 2 ]] || { echo 'usage: test-stategraph-canonical-then-native.sh LLVM_COMPILER CLANG' >&2; exit 64; }
repo="$(cd "$(dirname "$0")/.." && pwd -P)"
compiler="$1"
clang="$2"
[[ -x "$compiler" && -f "$compiler" ]] || { echo 'LLVM_SOURCE_COMPILER_MISSING' >&2; exit 1; }
[[ -x "$clang" && -f "$clang" ]] || { echo 'CLANG_MISSING' >&2; exit 1; }
command -v timeout >/dev/null || { echo 'TIMEOUT_MISSING' >&2; exit 1; }
command -v sha256sum >/dev/null || { echo 'SHA256SUM_MISSING' >&2; exit 1; }
cd "$repo"
positive=contracts/stategraph-portability/shared-canonical-then.ores
negative=tests/fixtures/stategraph-canonical-then-assignment-reject.ores
[[ -f "$positive" && ! -L "$positive" && -f "$negative" && ! -L "$negative" ]]
[[ "$(git hash-object "$positive")" == 'ad42a13d1542d8df6215bff42d056a489015c842' ]] ||
  { echo 'CANONICAL_THEN_GIT_BLOB_MISMATCH' >&2; exit 1; }
temp="$(mktemp -d)"
trap 'rm -rf "$temp"' EXIT
chmod 700 "$temp"
timeout --kill-after=3s 45s "$compiler" --target=host "$positive" > "$temp/positive.ll" 2> "$temp/compiler.err" ||
  { cat "$temp/compiler.err" >&2; echo 'CANONICAL_THEN_FRONTEND_NOT_ACCEPTED' >&2; exit 1; }
[[ -s "$temp/positive.ll" ]] || { echo 'LLVM_IR_MISSING' >&2; exit 1; }
timeout --kill-after=3s 45s "$clang" -O2 -x ir "$temp/positive.ll" -o "$temp/guest" ||
  { echo 'CANONICAL_THEN_CLANG_LINK_FAILED' >&2; exit 1; }
set +e
timeout --kill-after=3s 25s "$temp/guest" > "$temp/stdout" 2> "$temp/stderr"
guest_status=$?
set -e
[[ "$guest_status" -eq 42 && ! -s "$temp/stdout" && ! -s "$temp/stderr" ]] ||
  { cat "$temp/stderr" >&2; echo "CANONICAL_THEN_GUEST_FAILED status=$guest_status" >&2; exit 1; }
set +e
timeout --kill-after=3s 25s "$compiler" --target=host "$negative" > "$temp/negative.ll" 2> "$temp/negative.err"
negative_status=$?
set -e
[[ "$negative_status" -ne 0 && ! -s "$temp/negative.ll" ]] ||
  { echo 'CANONICAL_THEN_ASSIGNMENT_NOT_FAIL_CLOSED' >&2; exit 1; }
printf 'LLVM_CANONICAL_THEN_CTEST_PASS compiler_sha256=%s guest_exit=42 source_blob=%s\n' \
  "$(sha256sum "$compiler" | cut -d ' ' -f1)" \
  "$(git hash-object "$positive")"
