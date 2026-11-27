#!/usr/bin/env bash
# Rust-hosted native LLVM compiler lane; real ELF execution, not a host oracle.
set -euo pipefail
cd "$(dirname "$0")/.."
compiler=target/release/atto-llvm
test -x "$compiler"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
positive=0
negative=0
pass() {
  printf '%s\n' "$1" > "$work/input.atto"
  rm -f "$work/guest" "$work/guest.ll" "$work/repeat.ll"
  "$compiler" "$work/input.atto" -o "$work/guest.ll" --emit-llvm
  "$compiler" "$work/input.atto" -o "$work/repeat.ll" --emit-llvm
  cmp "$work/guest.ll" "$work/repeat.ll"
  clang -x ir -O2 -o "$work/guest" "$work/guest.ll"
  local status
  set +e
  timeout 15s "$work/guest"
  status=$?
  set -e
  test "$status" = 42
  positive=$((positive+1))
}
reject() {
  printf '%s\n' "$1" > "$work/input.atto"
  rm -f "$work/rejected.ll"
  if "$compiler" "$work/input.atto" -o "$work/rejected.ll" --emit-llvm >"$work/rejected.log" 2>&1; then
    echo "negative compiled" >&2; exit 1
  fi
  test ! -e "$work/rejected.ll"
  grep -q "$2" "$work/rejected.log"
  negative=$((negative+1))
}
pass 'pub fnc main(): int { return match 2 over in [0,1,2] -> 42, on _ -> 0 end; }'
pass 'pub fnc main(): int { return match 7 over in [0,1,2] -> 0, on _ -> 42 end; }'
pass 'pub fnc main(): int { return match 7 over is in [int,string] -> 42 end; }'
pass 'pub fnc main(): int { return match true over is in [int,bool] -> 42 end; }'
pass 'pub fnc main(): int { return match 2 over on 0 -> 0, is in [int,bool] -> 42 end; }'
pass 'pub fnc main(): int { return match 2 over is in [int,bool] when false -> 0, is int -> 42 end; }'
pass 'pub fnc main(): int { return match 2 over on 2 -> 42, in [1,2,3] -> 0, on _ -> 0 end; }'
pass 'pub fnc main(): int { return match 2 over in [1,2] when false -> 0, in [1,2] -> 42, on _ -> 0 end; }'
pass 'pub fnc main(): int { return match (-9223372036854775807 - 1) over in [-9223372036854775808,0] -> 42, on _ -> 0 end; }'
pass 'fnc get(): int { return 2; } pub fnc main(): int { return match get() over in [1,2] -> 42, on _ -> 0 end; }'
reject 'pub fnc main(): int { return match 2 over in [] -> 42, on _ -> 0 end; }' E_MATCH
reject 'pub fnc main(): int { return match 2 over in [2,2] -> 42, on _ -> 0 end; }' E_MATCH
reject 'pub fnc main(): int { return match 2 over in [1,2] -> 42, on 2 -> 0, on _ -> 0 end; }' E_MATCH
reject 'pub fnc main(): int { return match 2 over in [1,2] -> 42 end; }' E_MATCH
reject 'pub fnc main(): int { return match 2 over in values -> 42, on _ -> 0 end; }' E_UNSUPPORTED
reject 'pub fnc main(): int { return match 7 over is in [] -> 42 end; }' E_SYNTAX
reject 'pub fnc main(): int { return match 7 over is in [int,int] -> 42 end; }' E_MATCH
reject 'pub fnc main(): int { return match 7 over is in [string,String] -> 42 end; }' E_MATCH
reject 'pub fnc main(): int { return match 7 over is in [int,42] -> 42 end; }' E_UNSUPPORTED
reject 'pub fnc main(): int { return match 7 over is in [int,bool] -> 42, on _ -> 0 end; }' E_MATCH
test "$positive" = 10
test "$negative" = 10
echo "INT_MEMBERSHIP_NATIVE_GUEST_PASS positive=$positive negative=$negative"
