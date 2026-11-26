#!/usr/bin/env bash
# Real LLVM compiler and native executable; admission only, not codec parity.
set -euo pipefail
compiler="$(realpath -- "${1:?pass compiled oreslang-llvmc}")"
test -x "$compiler"
work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT
printf '%s\n' 'pub fnc main(): int { /* Serializable to_json() */ return 42; }' > "$work/control.atto"
"$compiler" "$work/control.atto" > "$work/control.ll" 2> "$work/control.err"
test -s "$work/control.ll"
clang -x ir "$work/control.ll" -o "$work/control"
rc=0; "$work/control" || rc=$?
test "$rc" = 42
reject() {
  local label="$1" source="$2" rc=0
  printf '%s\n' "$source" > "$work/$label.atto"
  "$compiler" "$work/$label.atto" > "$work/$label.ll" 2> "$work/$label.err" || rc=$?
  test "$rc" = 1
  test ! -s "$work/$label.ll"
  grep -Eq '^oreslang-llvmc: .+' "$work/$label.err"
  grep -Eiq 'unsupported|expected|unexpected|unknown|undefined|unbound|identifier|declaration|nominal|not supported' "$work/$label.err"
  cat "$work/$label.err"
  echo "LLVM_SERDE_ADMISSION_DENIED $label"
}
reject missing 'pub fnc main(): int { return deserialize; }'
reject class 'define class User as pub to_json(): string { return "x"; } end pub fnc main(): int { return 0; }'
reject struct 'define struct User implements Serializable as value: int; end pub fnc main(): int { return 0; }'
reject interface 'define interface JsonSerializable as to_json(): string; end pub fnc main(): int { return 0; }'
reject trait 'define trait JsonDeserializable as from_json(string input): string; end pub fnc main(): int { return 0; }'
echo 'LLVM_TYPED_SERDE_ADMISSION_PASS checks=6 NOT_SERIALIZER_EXECUTION'
