#!/usr/bin/env bash
# Real Oreslang guest compiler -> LLVM -> Clang; checked signed quotient.
set -euo pipefail
[[ $# == 2 ]] || { echo "usage: verify-checked-i64-quotient.sh LLVMC CLANG" >&2; exit 64; }
compiler="$1"; clang="$2"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
test -x "$compiler" || { echo "compiler missing" >&2; exit 69; }
command -v "$clang" >/dev/null || { echo "clang missing" >&2; exit 69; }
work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT
compile() {
  ORESLANG_LLVMC="$compiler" CLANG="$clang" \
    bash "$root/scripts/compile-aot.sh" "$1" "$2"
}
pass="$root/tests/fixtures/native-checked-i64-quotient.ores"
compile "$pass" "$work/good"
set +e
"$work/good" >"$work/good.out" 2>"$work/good.err"
rc=$?
set -e
if [[ $rc -ne 42 || -s "$work/good.out" || -s "$work/good.err" ]]; then
  echo "valid checked quotient fixture failed: exit=$rc (expected 42)" >&2
  cat "$work/good.out" "$work/good.err" >&2
  exit 1
fi
# A wrong-typed quotient must be rejected by the actual compiler and emit no IR.
badtype="$root/tests/fixtures/native-checked-i64-quotient-type-reject.ores"
if "$compiler" "$badtype" >"$work/type.ll" 2>"$work/type.err"; then
  echo "type-invalid quotient was accepted" >&2; exit 1
fi
if [[ -s "$work/type.ll" ]] || ! grep -q '^oreslang-llvmc:' "$work/type.err"; then
  echo "type error did not fail closed without partial LLVM IR" >&2
  cat "$work/type.err" >&2; exit 1
fi
# Invalid operands compile to guarded IR but must never reach poison-producing
# sdiv: the checked-arithmetic trap is required and must be a signal.
ulimit -c 0 || true
for label in zero overflow; do
  src="$root/tests/fixtures/native-checked-i64-quotient-$label.ores"
  compile "$src" "$work/$label"
  set +e
  ( "$work/$label" >"$work/$label.out" 2>"$work/$label.err" ) 2>/dev/null
  rc=$?
  set -e
  if [[ $rc -lt 128 || -s "$work/$label.out" ]]; then
    echo "$label invalid quotient did not trap: exit=$rc" >&2
    cat "$work/$label.out" "$work/$label.err" >&2; exit 1
  fi
done
sha256sum "$pass" "$compiler" "$work/good"
echo 'CHECKED-I64-QUOTIENT-GUEST-LLVM-OK'
