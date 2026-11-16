#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
compiler="${1:-./build/oreslang-llvmc}"
command -v clang >/dev/null
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
for name in return-get scalar-copy return-early return-fallthrough; do
  "$compiler" --experimental-owned-int "tests/atto-box-main-v1/$name.atto" > "$tmp/$name.ll"
  # Declarations/references alone are not evidence of owned region use.
  # Require actual call sites in each emitted guest IR program: allocation,
  # read, drop and close. This is independent from the exit-42 oracle.
  for operation in new read drop close; do
    if ! grep -Eq "^[[:space:]]*(%[^[:space:]=]+[[:space:]]*=[[:space:]]*)?((tail|musttail|notail)[[:space:]]+)?call[[:space:]]+[^;]*@oreslang_owned_i64_region_${operation}[[:space:]]*\(" "$tmp/$name.ll"; then
      echo "Box AOT $name missing executed-region call: $operation" >&2
      exit 1
    fi
  done
  clang_flags=(-O1 -g -fno-omit-frame-pointer)
  if [ "${ATTO_BOX_SANITIZE:-0}" = "1" ]; then
    clang_flags+=(-fsanitize=address,undefined)
  fi
  clang "${clang_flags[@]}" -x ir "$tmp/$name.ll" -x c src/main/cpp/oreslang/runtime/NativeOwnedI64Region.c -Isrc/main/cpp -std=c11 -o "$tmp/$name"
  if "$tmp/$name"; then status=0; else status=$?; fi
  test "$status" -eq 42 || { echo "Box AOT $name returned $status" >&2; exit 1; }
done
negative_count=0
for fixture in tests/atto-box-main-v1/reject-*.atto; do
  # A compiler crash or an unrelated parser error is NOT an ownership proof.
  # Require a normal diagnostic exit and an explicit safety error category.
  status=0
  "$compiler" --experimental-owned-int "$fixture" > "$tmp/rejected.ll" 2>"$tmp/rejected.err" || status=$?
  if [ "$status" -eq 0 ]; then
    echo "unsafe Box source compiled: $fixture" >&2; exit 1
  fi
  if [ "$status" -ne 1 ] && [ "$status" -ne 2 ]; then
    echo "Box negative fixture had abnormal compiler exit $status: $fixture" >&2
    cat "$tmp/rejected.err" >&2
    exit 1
  fi
  test ! -s "$tmp/rejected.ll" || {
    echo "Box negative fixture emitted LLVM IR: $fixture" >&2; exit 1;
  }
  case "$fixture" in
    *reject-actor-name.atto|*reject-alias-name.atto)
      category=NATIVE_OWNERSHIP_INTRINSIC_RESERVED ;;
    *)
      category=ATTO_BOX_MAIN_V1_REJECT ;;
  esac
  if ! grep -Fq "$category" "$tmp/rejected.err"; then
    echo "Box negative fixture lacked expected semantic diagnostic $category: $fixture" >&2
    cat "$tmp/rejected.err" >&2
    exit 1
  fi
  negative_count=$((negative_count + 1))
done
test "$negative_count" -eq 10 || {
  echo "Box negative coverage changed: expected 10, observed $negative_count" >&2
  exit 1
}
echo "ATTO_BOX_MAIN_V1_NEGATIVE_10_EXPLICIT_DIAGNOSTICS_PASS"
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
