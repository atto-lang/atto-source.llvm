#!/usr/bin/env bash
# ACTUAL LLVM AttoLang .atto frontend -> LLVM IR -> Clang executable guest run.
# No mock compiler, Python shim, or scripted expected-output backend.
# Scope: i64 shift operators used for ODB1 byte packing; full WAL codec remains
# unsupported by the native Oreslang source compiler until List/Result parity.
set -euo pipefail
export LC_ALL=C
ulimit -c 0 || true
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
compiler="${ORESLANG_LLVMC:-$root/build/oreslang-llvmc}"
clang="${CLANG:-clang}"
case "$compiler" in /*) ;; *) echo 'ORESLANG_LLVMC must be an absolute path' >&2; exit 2;; esac
test -x "$compiler" || { echo "missing real compiler $compiler" >&2; exit 2; }
command -v "$clang" >/dev/null || { echo 'missing clang linker' >&2; exit 2; }
command -v timeout >/dev/null || { echo 'missing timeout' >&2; exit 2; }
work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT
compile_guest() {
  local name="$1"
  local source="$root/tests/fixtures/$name.atto"
  test -f "$source" || { echo "fixture missing: $source" >&2; exit 2; }
  timeout 40s "$compiler" --target=host "$source" > "$work/$name.ll" 2>"$work/$name.err"
  test -s "$work/$name.ll" || { echo "empty LLVM IR $name" >&2; exit 1; }
  "$clang" -O2 -x ir "$work/$name.ll" -o "$work/$name.bin" 2>"$work/$name.link.err"
  test -x "$work/$name.bin"
}
compile_guest odb1-bitshift-parity-positive
# Verify the real native frontend emitted THREE distinct LLVM shifts, with
# pre-shift distance checking. Do not silently rewrite >> to divide by 256.
for pattern in 'ashr i64' 'lshr i64' 'shl i64' 'icmp ult i64' 'llvm.trap'; do
  grep -q -- "$pattern" "$work/odb1-bitshift-parity-positive.ll" || {
    echo "native IR missing required shift/guard: $pattern" >&2; exit 1;
  }
done
if ! timeout 20s "$work/odb1-bitshift-parity-positive.bin"     >"$work/positive.stdout" 2>"$work/positive.stderr"; then
  echo 'real native ODB1 scalar shift positive fixture failed' >&2
  cat "$work/positive.stderr" >&2
  exit 1
fi
test ! -s "$work/positive.stdout" || { echo 'unexpected guest stdout' >&2; exit 1; }
echo 'NATIVE_ODB1_SHIFT_POSITIVE_PASS assertions=13; REAL_GUEST_EXECUTED'
# These programs are TYPE/GRAMMAR errors, not user-visible successful IR.
for name in odb1-bitshift-invalid-boolean-operand             odb1-bitshift-invalid-boolean-distance             odb1-bitshift-invalid-nonadjacent; do
  source="$root/tests/fixtures/$name.atto"
  if timeout 40s "$compiler" --target=host "$source"       >"$work/$name.ll" 2>"$work/$name.err"; then
    echo "unsupported native guest unexpectedly compiled: $name" >&2; exit 1
  fi
  test ! -s "$work/$name.ll" || {
    echo "compiler published LLVM IR before rejecting $name" >&2; exit 1;
  }
  test -s "$work/$name.err" || { echo "missing rejection diagnostic: $name" >&2; exit 1; }
  echo "NATIVE_ODB1_SHIFT_COMPILE_REJECTION_PASS $name"
done
# The Java reference requires distances 0..63. LLVM shifts at >=64 create
# poison; negative distances must trap too. A normal numeric exit here is
# NOT proof of runtime checking. All three fixtures must signal after building.
for name in odb1-bitshift-invalid-negative-distance             odb1-bitshift-invalid-64-distance             odb1-bitshift-invalid-dynamic-distance             odb1-bitshift-invalid-unsigned-distance; do
  compile_guest "$name"
  rc=0
  timeout 20s "$work/$name.bin" >"$work/$name.stdout"       2>"$work/$name.runtime.err" || rc=$?
  if (( rc < 128 || rc == 124 || rc == 125 || rc == 126 || rc == 127 )); then
    echo "missing native shift-distance runtime trap: $name status=$rc" >&2
    exit 1
  fi
  echo "NATIVE_ODB1_SHIFT_RUNTIME_REJECTION_PASS $name status=$rc"
done
echo 'NATIVE_ODB1_SHIFT_SOURCE_EXECUTION_PASS positive=13 compile_rejections=3 runtime_traps=4; NOT FULL DB CODEC PARITY'
