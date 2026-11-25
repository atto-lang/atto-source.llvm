#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd -P)"
suite="$root/tests/atto-core-numeric-v1"
header="$root/src/main/cpp/oreslang/frontend/GeneratedNumericAtto.inc"
source="$root/stdlib/numeric.atto"
compiler="$root/build/oreslang-llvmc"
auditor="$root/build/oreslang-no-gc-ir-audit"
mode="--run"
if [[ $# -gt 0 ]]; then mode="$1"; fi
die() { echo "FAIL AttoLang LLVM numeric: $*" >&2; exit 1; }
[[ "$mode" == --run || "$mode" == --source-only ]] || die "unknown mode"
[[ -f "$source" && ! -L "$source" && -f "$header" && ! -L "$header" ]] || die "canonical source/header absent"
command -v git >/dev/null || die "git required"
command -v cmp >/dev/null || die "cmp required"
expected="5ced991f1d49b3c2a60fdd72b9003c0a4d18cd72"
test "$(git hash-object -- "$source")" = "$expected" || die "Java canonical blob hash mismatch"
tmp="$(mktemp -d)"
trap 'rm -rf -- "$tmp"' EXIT
command -v python3 >/dev/null || die "Python3 required for canonical numeric embed verification"
python3 "$root/scripts/generate_atto_numeric_embed.py" --check >/dev/null || die "numeric .atto embed differs from compiler literal"
python3 -m unittest discover -s "$root/tests" -p "test_atto_numeric_embed.py" -q || die "numeric source-derived embed adversarial tests failed"
command -v c++ >/dev/null || die "C++17 compiler required for derived-literal syntax test"
c++ -std=c++17 -Wall -Wextra -Werror -I"$root/src/main/cpp" "$root/tests/atto_numeric_embed_cpp17_probe.cpp" -o "$tmp/atto-embed-cpp17" || die "generated .atto C++17 include failed to compile"
"$tmp/atto-embed-cpp17" || die "generated .atto C++17 include probe failed"
for name in canonical legacy mixed reject-double-extension reject-host-override; do
  [[ -f "$suite/$name.atto" && ! -L "$suite/$name.atto" ]] || die "missing fixture $name"
done
if [[ "$mode" == --source-only ]]; then
  echo "ATTO_LLVM_NUMERIC_SOURCE_ONLY|sha=$expected|compiler=NOT_EXECUTED"
  exit 0
fi
[[ -x "$compiler" && ! -L "$compiler" ]] || die "real LLVM compiler not built"
[[ -x "$auditor" && ! -L "$auditor" ]] || die "real no-GC IR auditor not built"
command -v clang >/dev/null || die "real clang required"
command -v timeout >/dev/null || die "timeout required"
receipts="$tmp/receipts"
: > "$receipts"
for name in canonical legacy mixed; do
  for opt in 0 2; do
    ir="$tmp/$name-O$opt.ll"
    exe="$tmp/$name-O$opt"
    set +e
    timeout 60s "$compiler" --target=host "$suite/$name.atto" > "$ir" 2> "$tmp/compile.err"
    status=$?
    set -e
    [[ "$status" -eq 0 && -s "$ir" ]] || die "compiler failed $name O$opt status=$status: $(head -c 1000 "$tmp/compile.err")"
    grep -Eq 'define[[:space:]]+i32[[:space:]]+@main\(' "$ir" || die "main absent: $name O$opt"
    if grep -Fq '__ores_module_2.' "$ir"; then die "duplicate core alias: $name O$opt"; fi
    timeout 20s "$auditor" "$ir" >/dev/null || die "no-GC audit failed: $name O$opt"
    timeout 60s clang "-O$opt" -x ir "$ir" -o "$exe" || die "Clang link failed: $name O$opt"
    set +e
    timeout 10s "$exe" > "$tmp/stdout" 2> "$tmp/stderr"
    status=$?
    set -e
    [[ "$status" -eq 42 && ! -s "$tmp/stdout" && ! -s "$tmp/stderr" ]] || die "native guest wrong exit/output: $name O$opt ($status)"
    printf 'ATTO_LLVM_NUMERIC_NATIVE_HOST_OBSERVED|%s|O%s|exit=42|guest=executed\n' "$name" "$opt" >> "$receipts"
  done
  cmp -s "$tmp/$name-O0.ll" "$tmp/$name-O2.ll" || die "LLVM compiler IR differs across optimization levels: $name"
done
for name in reject-double-extension reject-host-override; do
  ir="$tmp/$name.ll"
  set +e
  timeout 30s "$compiler" --target=host "$suite/$name.atto" > "$ir" 2> "$tmp/negative.err"
  status=$?
  set -e
  [[ "$status" -gt 0 && "$status" -lt 124 && ! -s "$ir" && -s "$tmp/negative.err" ]] || die "invalid negative result: $name exit=$status"
  printf 'ATTO_LLVM_NUMERIC_NEGATIVE_HOST_OBSERVED|%s|rejected=1|partial_ir=0\n' "$name" >> "$receipts"
done
test "$(git hash-object -- "$source")" = "$expected" || die "source drift during execution"
python3 "$root/scripts/generate_atto_numeric_embed.py" --check >/dev/null || die "generated numeric embed drift during execution"
cat "$receipts"
echo "ATTO_LLVM_NUMERIC_GUEST_PARITY_EXECUTED|positive=3|optimizations=2|native=6|negative=2|source_blob=$expected"
