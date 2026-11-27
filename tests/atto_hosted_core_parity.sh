#!/usr/bin/env bash
# Native hosted std/core.atto guest execution; source-only checks are weaker.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd -P)"
suite="$root/tests/atto-hosted-core-v1"
header="$root/src/main/cpp/oreslang/frontend/CoreLibrarySources.hpp"
source="$root/stdlib/core.atto"
compiler="$root/build/oreslang-llvmc"
auditor="$root/build/oreslang-no-gc-ir-audit"
mode="--run"
if [[ $# -gt 0 ]]; then mode="$1"; fi
die() { echo "FAIL AttoLang LLVM hosted core: $*" >&2; exit 1; }
[[ "$mode" == --run || "$mode" == --source-only ]] || die "expected --run or --source-only"
[[ -f "$source" && ! -L "$source" && -f "$header" && ! -L "$header" ]] || die "nonregular canonical std/core.atto or compiler header"
command -v git >/dev/null || die "git required"
command -v cmp >/dev/null || die "cmp required"
expected="d2cd9b8e0cd3e3bb3ee6a5f9cebf9ab3312c44f4"
test "$(git hash-object -- "$source")" = "$expected" || die "canonical Java/GraalVM std/core.atto blob differs"
tmp="$(mktemp -d)"
trap 'rm -rf -- "$tmp"' EXIT
awk '/^R"ATTO_HOSTED_CORE\(/ {copy=1;next} copy && /^\)ATTO_HOSTED_CORE";/ {done=1;exit} copy {print} END {if (!done) exit 9}' "$header" > "$tmp/embedded.atto" ||
  die "missing compiler-owned hosted core raw-string boundary"
cmp -s "$source" "$tmp/embedded.atto" || die "LLVM built-in module not byte-identical to canonical .atto"
for name in canonical legacy mixed reject-double-extension reject-path-traversal reject-host-java; do
  [[ -f "$suite/$name.atto" && ! -L "$suite/$name.atto" ]] || die "missing/nonregular source $name"
done
if [[ "$mode" == --source-only ]]; then
  echo "ATTO_LLVM_HOSTED_CORE_SOURCE_ONLY|sha=$expected|compiler=NOT_EXECUTED"
  exit 0
fi
[[ -x "$compiler" && ! -L "$compiler" ]] || die "real LLVM compiler missing"
[[ -x "$auditor" && ! -L "$auditor" ]] || die "real LLVM no-GC IR auditor missing"
command -v clang >/dev/null || die "Clang required"
command -v timeout >/dev/null || die "timeout required"
receipts="$tmp/receipts"
: > "$receipts"
for name in canonical legacy mixed; do
  for opt in 0 2; do
    ir="$tmp/$name-O$opt.ll"
    exe="$tmp/$name-O$opt"
    set +e
    timeout 60s "$compiler" --target=host "$suite/$name.atto" > "$ir" 2> "$tmp/compile.err"
    compiled=$?
    set -e
    [[ "$compiled" -eq 0 && -s "$ir" ]] || die "LLVM compiler failed: $name O$opt status=$compiled: $(head -c 1200 "$tmp/compile.err")"
    grep -Eq 'define[[:space:]]+i32[[:space:]]+@main\(' "$ir" || die "native main absent: $name O$opt"
    if grep -Fq '__ores_module_2.' "$ir"; then die "duplicate logical unit: $name O$opt"; fi
    timeout 20s "$auditor" "$ir" > /dev/null || die "no-GC IR audit failed: $name O$opt"
    timeout 60s clang "-O$opt" -x ir "$ir" -o "$exe" || die "Clang failed: $name O$opt"
    [[ -x "$exe" && -f "$exe" && ! -L "$exe" ]] || die "missing native executable: $name O$opt"
    set +e
    timeout 10s "$exe" > "$tmp/guest.out" 2> "$tmp/guest.err"
    status=$?
    set -e
    [[ "$status" -eq 42 && ! -s "$tmp/guest.out" && ! -s "$tmp/guest.err" ]] ||
      die "bad native guest output/exit $name O$opt status=$status"
    printf 'ATTO_LLVM_HOSTED_CORE_NATIVE_HOST_OBSERVED|%s|O%s|exit=42|guest=executed|audit=passed\n' "$name" "$opt" >> "$receipts"
  done
  cmp -s "$tmp/$name-O0.ll" "$tmp/$name-O2.ll" || die "compiler IR changed unexpectedly across optimization levels: $name"
done
for name in reject-double-extension reject-path-traversal reject-host-java; do
  ir="$tmp/$name.ll"
  set +e
  timeout 30s "$compiler" --target=host "$suite/$name.atto" > "$ir" 2> "$tmp/reject.err"
  status=$?
  set -e
  [[ "$status" -gt 0 && "$status" -lt 124 && ! -s "$ir" && -s "$tmp/reject.err" ]] ||
    die "invalid negative compiler admission: $name status=$status"
  printf 'ATTO_LLVM_HOSTED_CORE_NEGATIVE_HOST_OBSERVED|%s|rejected=1|partial_ir=0\n' "$name" >> "$receipts"
done
set +e
timeout 30s "$compiler" --target=thumbv7m-none-eabi --library-profile=core "$suite/canonical.atto" > "$tmp/freestanding.ll" 2> "$tmp/freestanding.err"
status=$?
set -e
[[ "$status" -gt 0 && "$status" -lt 124 && ! -s "$tmp/freestanding.ll" && -s "$tmp/freestanding.err" ]] ||
  die "hosted std/core illegally admitted into freestanding target"
echo "ATTO_LLVM_HOSTED_CORE_NEGATIVE_HOST_OBSERVED|freestanding|rejected=1|partial_ir=0" >> "$receipts"
test "$(git hash-object -- "$source")" = "$expected" || die "canonical guest source changed during run"
cmp -s "$source" "$tmp/embedded.atto" || die "embedded compiler source changed during run"
cat "$receipts"
echo "ATTO_LLVM_HOSTED_CORE_GUEST_PARITY_EXECUTED|positive=3|optimizations=2|native=6|negative=4|source_blob=$expected"
