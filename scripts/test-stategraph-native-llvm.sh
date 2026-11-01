#!/usr/bin/env bash
# Native compiled Oreslang guest proof (LLVM frontend -> Clang executable).
set -euo pipefail
cd "$(dirname "$0")/.."
[[ "$(git rev-parse HEAD)" = "${EXPECTED_SOURCE_SHA:?exact head required}" ]]
[[ -x build/oreslang-llvmc ]] || { echo 'NATIVE_LLVM_COMPILER_NOT_AVAILABLE' >&2; exit 1; }
command -v clang >/dev/null
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
echo "SG_LLVM_BEGIN head=$(git rev-parse HEAD) tree=$(git rev-parse 'HEAD^{tree}') compiler_sha256=$(sha256sum build/oreslang-llvmc | cut -d' ' -f1)"
shared=contracts/stategraph-portability/shared-scalar.ores
[[ "$(git hash-object "$shared")" = "ec1cb5e6cc38c26842d724ccacb0df028b702c3f" ]] || {
  echo 'SG_LLVM_SHARED_SCALAR_SOURCE_MISMATCH' >&2; exit 1;
}
git ls-files --error-unmatch -- "$shared" >/dev/null
[[ -f "$shared" && ! -L "$shared" ]]
timeout --kill-after=3s 90s bash scripts/compile-aot.sh "$shared" "$tmp/common"
[[ -x "$tmp/common" ]]
set +e
timeout --kill-after=3s 30s "$tmp/common" > "$tmp/out" 2> "$tmp/err"
common_status=$?
set -e
if [[ "$common_status" -ne 42 || -s "$tmp/out" ]]; then
  cat "$tmp/err" >&2
  echo "SG_LLVM_COMMON_SCALAR_NOT_ACCEPTED status=$common_status" >&2
  exit 1
fi
printf 'SG_LLVM_COMMON_SCALAR_NATIVE_PASS blob=%s guest_exit=42\n' "$(git rev-parse "HEAD:$shared")"

# First byte-identical generic userland StateGraph source.
generic=contracts/stategraph-portability/shared-generic-state.ores
[[ -f "$generic" && ! -L "$generic" ]]
git ls-files --error-unmatch -- "$generic" >/dev/null
[[ "$(git hash-object "$generic")" = "c3a67b837a73f427a6157c446cc6e3989e9a66da" ]] ||
  { echo 'SG_LLVM_GENERIC_SOURCE_DRIFT' >&2; exit 1; }
[[ "$(git rev-parse "HEAD:$generic")" = "c3a67b837a73f427a6157c446cc6e3989e9a66da" ]]
timeout --kill-after=3s 90s bash scripts/compile-aot.sh "$generic" "$tmp/generic" ||
  { echo 'SG_LLVM_GENERIC_LOWERING_NOT_ACCEPTED' >&2; exit 1; }
[[ -x "$tmp/generic" ]]
set +e
timeout --kill-after=3s 30s "$tmp/generic" > "$tmp/out" 2> "$tmp/err"
generic_status=$?
set -e
if [[ "$generic_status" -ne 42 || -s "$tmp/out" ]]; then
  cat "$tmp/err" >&2
  echo "SG_LLVM_GENERIC_GUEST_NOT_ACCEPTED status=$generic_status" >&2
  exit 1
fi
printf 'SG_LLVM_GENERIC_NATIVE_GUEST_PASS blob=%s result=42\n' "$(git rev-parse "HEAD:$generic")"

for name in scalar guard nominal; do
  source="tests/fixtures/stategraph-parity-$name.ores"
  git ls-files --error-unmatch "$source" >/dev/null
  [[ -s "$source" && ! -L "$source" ]]
  timeout --kill-after=3s 90s bash scripts/compile-aot.sh "$source" "$tmp/$name" ||
    { echo "SG_LLVM_COMPILE_NOT_ACCEPTED $source" >&2; exit 1; }
  [[ -x "$tmp/$name" ]]
  set +e
  timeout --kill-after=3s 30s "$tmp/$name" > "$tmp/out" 2> "$tmp/err"
  status=$?
  set -e
  if [[ "$status" -ne 42 || -s "$tmp/out" ]]; then
    cat "$tmp/err" >&2
    echo "SG_LLVM_GUEST_NOT_ACCEPTED $name status=$status" >&2
    exit 1
  fi
  printf 'SG_LLVM_GUEST_PASS name=%s blob=%s guest_exit=42\n' \
    "$name" "$(git rev-parse "HEAD:$source")"
done
echo 'SG_LLVM_NATIVE_GUEST_PASS count=3 (NOT full StateGraph or actor parity)'
