#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
backend=${1:-native}
case "$backend" in
  native) compiler=target/release/atto-llvm ;;
  wasm) compiler=frontend/target/release/attoc ;;
  *) exit 2 ;;
esac
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
ok() {
  local name="$1"; shift
  printf '%s\n' "$1" > "$work/$name.atto"
  if [[ "$backend" == native ]]; then
    "$compiler" "$work/$name.atto" -o "$work/$name.elf"
    readelf -h "$work/$name.elf" | grep -q ELF64
    local status=0
    "$work/$name.elf" || status=$?
    test "$status" = 42
  else
    "$compiler" "$work/$name.atto" -o "$work/$name.wasm"
    node --input-type=module -e '
      import {readFileSync} from "node:fs";
      import assert from "node:assert/strict";
      const {instance}=await WebAssembly.instantiate(readFileSync(process.argv[1]),{});
      assert.equal(instance.exports.main(),42n);
      assert.equal(instance.exports.panic_code.value,0);
    ' "$work/$name.wasm"
  fi
  echo "PASS signed division $backend $name"
}
bad() {
  local name="$1"; shift
  printf '%s\n' "$1" > "$work/$name.atto"
  local output="$work/$name.$backend"
  if "$compiler" "$work/$name.atto" -o "$output" > "$work/log" 2>&1; then
    cat "$work/log"; exit 1
  fi
  test ! -e "$output"
  grep -F 'E_UNSUPPORTED' "$work/log"
  echo "PASS no artifact $name"
}
ok truncation 'pub fnc main():int {return 45 + (-7 / 2);}'
ok negative-divisor 'pub fnc main():int {return 45 + (7 / -2);}'
ok precedence 'pub fnc main():int {return 39 + 7 / 2;}'
ok dynamic-numerator 'fnc divide(int n):int{return n / 2;} pub fnc main():int{return divide(85);}'
ok minimum-safe 'pub fnc main():int{guard -9223372036854775808 / 2 eq -4611686018427387904 {return 1;} return 42;}'
ok negative-one 'pub fnc main():int{return -42 / -1;}'
bad zero 'pub fnc main():int{return 42 / 0;}'
bad overflow 'pub fnc main():int{return -9223372036854775808 / -1;}'
bad dynamic-divisor 'fnc divide(int n,int d):int{return n/d;} pub fnc main():int{return divide(42,1);}'
bad dynamic-minus-one 'fnc divide(int n):int{return n/-1;} pub fnc main():int{return divide(-42);}'
