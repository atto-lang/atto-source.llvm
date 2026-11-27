#!/usr/bin/env bash
# Focused actual native LLVM scalar copy qualification; no heap-copy claim.
set -euo pipefail
cd "$(dirname "$0")/.."
cargo build --locked --offline --release --bin atto-llvm
compiler="$PWD/target/release/atto-llvm"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
positive() {
  local id="$1" source="$2"
  printf '%s\n' "$source" > "$work/$id.atto"
  "$compiler" "$work/$id.atto" -o "$work/$id.ll" --emit-llvm
  "$compiler" "$work/$id.atto" -o "$work/$id.again.ll" --emit-llvm
  cmp "$work/$id.ll" "$work/$id.again.ll"
  # Both optimizer levels run genuine linked native guest executables.
  for opt in 0 2; do
    clang -x ir -O"$opt" -o "$work/$id-O$opt.exe" "$work/$id.ll"
    readelf -h "$work/$id-O$opt.exe" | grep -q 'Class:[[:space:]]*ELF64'
    local status=0
    timeout 10s "$work/$id-O$opt.exe" || status=$?
    test "$status" = 42
  done
  echo "PASS native scalar copy $id O0/O2"
}
negative() {
  local id="$1" code="$2" source="$3" status=0
  printf '%s\n' "$source" > "$work/$id.atto"
  "$compiler" "$work/$id.atto" -o "$work/$id.ll" --emit-llvm >"$work/$id.log" 2>&1 || status=$?
  test "$status" = 1
  grep -Eq "(^|: )$code:" "$work/$id.log"
  test ! -e "$work/$id.ll"
  echo "PASS native copy denial $id $code"
}
positive modes 'pub fnc main(): int { let int a = 20; let int b = copy! a; let int c = copy!! a; a = 2; return a+b+c; }'
positive spaced 'pub fnc main(): int { let bool a = false; let bool b = copy! !a; let bool c = copy!! !a; return (b && c) ? 42 : 0; }'
positive precedence 'pub fnc main(): int { return copy! 20 + copy!! 22; }'
positive exact_once 'fnc value(): int { return 21; } pub fnc main(): int { return copy! value() + copy!! value(); }'
negative triple E_TYPE 'pub fnc main(): int { return copy!!! 42; }'
negative heap E_UNSUPPORTED_NATIVE_RUNTIME 'define class Box as pub let int n = 42; end pub fnc main(): int { let Box a = new Box(); let Box b = copy! a; return b.n; }'
echo "PASS NATIVE_COPY_SCALAR positives=4 native_executions=8 negatives=2"
