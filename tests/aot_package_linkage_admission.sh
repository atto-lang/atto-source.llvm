#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
driver="$root/scripts/compile-aot.sh"
w="$(mktemp -d)"; trap 'rm -rf "$w"' EXIT
printf 'previous valid output\n' > "$w/prior"
cp "$w/prior" "$w/output"
printf '#!/bin/sh\ntouch "%s"\nexit 99\n' "$w/invoked" > "$w/compiler"
chmod +x "$w/compiler"
for mode in dynamic hybrid unknown; do
  code=0
  ORESLANG_LLVMC="$w/compiler" bash "$driver" "$w/nonexistent.atto" "$w/output" "--link=$mode" 2>"$w/error" || code=$?
  [[ "$code" == 64 ]]
  if [[ "$mode" == unknown ]]; then grep -Fq 'E_LINK_MODE:' "$w/error"; else grep -Fq 'E_LINK_MODE_UNSUPPORTED:' "$w/error"; fi
  cmp "$w/prior" "$w/output"
  test ! -e "$w/invoked"
  code=0
  ORESLANG_LLVMC="$w/compiler" bash "$driver" "$w/nonexistent.atto" "$w/new" "--link=$mode" 2>"$w/error" || code=$?
  [[ "$code" == 64 && ! -e "$w/new" && ! -e "$w/invoked" ]]
done
echo 'PASS host driver denies unsupported/invalid modes without touching compiler or output'
if [[ "${1:-}" == --guest ]]; then
  printf 'pub fnc main(): int { return 42; }\n' > "$w/main.atto"
  bash "$driver" "$w/main.atto" "$w/default"
  bash "$driver" "$w/main.atto" "$w/static" --link=static
  for exe in default static; do
    code=0; "$w/$exe" || code=$?
    [[ "$code" == 42 ]]
  done
  echo 'PASS actual LLVM compiler native guest default and explicit static return 42'
else
  echo 'RECEIPT actual_guest=NOT_EXECUTED'
fi
