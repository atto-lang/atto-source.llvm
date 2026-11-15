#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
compiler="${1:-./build/oreslang-llvmc}"
command -v clang >/dev/null
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
for name in return-get scalar-copy; do
  "$compiler" --experimental-owned-int "tests/atto-box-main-v1/$name.atto" > "$tmp/$name.ll"
  grep -q 'oreslang_owned_i64_region_new' "$tmp/$name.ll"
  grep -q 'oreslang_owned_i64_region_read' "$tmp/$name.ll"
  grep -q 'oreslang_owned_i64_region_drop' "$tmp/$name.ll"
  grep -q 'oreslang_owned_i64_region_close' "$tmp/$name.ll"
  clang_flags=(-O1 -g -fno-omit-frame-pointer)
  if [ "${ATTO_BOX_SANITIZE:-0}" = "1" ]; then
    clang_flags+=(-fsanitize=address,undefined)
  fi
  clang "${clang_flags[@]}" -x ir "$tmp/$name.ll" -x c src/main/cpp/oreslang/runtime/NativeOwnedI64Region.c -Isrc/main/cpp -std=c11 -o "$tmp/$name"
  if "$tmp/$name"; then status=0; else status=$?; fi
  test "$status" -eq 42 || { echo "Box AOT $name returned $status" >&2; exit 1; }
done
for fixture in tests/atto-box-main-v1/reject-*.atto; do
  if "$compiler" --experimental-owned-int "$fixture" > "$tmp/rejected.ll" 2>"$tmp/rejected.err"; then
    echo "unsafe Box source compiled: $fixture" >&2; exit 1
  fi
  test ! -s "$tmp/rejected.ll"
done
if "$compiler" tests/atto-box-main-v1/return-get.atto >"$tmp/gate.ll" 2>"$tmp/gate.err"; then
  echo "Box admitted without experimental region permission" >&2; exit 1
fi
grep -q 'ATTO_BOX_MAIN_V1_REJECT' "$tmp/gate.err"
test ! -s "$tmp/gate.ll"
for target in wasm32 thumbv7m-none-eabi; do
  if "$compiler" --experimental-owned-int "--target=$target" tests/atto-box-main-v1/return-get.atto >"$tmp/target.ll" 2>"$tmp/target.err"; then
    echo "unverified $target Box target admitted" >&2; exit 1
  fi
  test ! -s "$tmp/target.ll"
done
if [ "${ATTO_BOX_SANITIZE:-0}" = "1" ]; then
  echo ATTO_MAIN_BOX_I64_AOT_SANITIZED_PASS
else
  echo ATTO_MAIN_BOX_I64_AOT_PASS
fi
