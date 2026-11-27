#!/usr/bin/env bash
set -euo pipefail
compiler="$1"
clang="$2"
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
runtime="$root/src/main/cpp/oreslang/runtime/NativeStringRegion.c"
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
printf AB > "$work/expected"
for entry in 'stdio-direct.atto fcb8c1b56322b1b068007513cd60791504de5918' 'process-direct.atto 0d4a07d1c1ccf6c9f1083816a3740091a8467641'; do
  read -r name blob <<< "$entry"
  source="$root/tests/fixtures/canonical-stdout-v1/$name"
  test "$(git hash-object "$source")" = "$blob"
  "$compiler" "$source" > "$work/guest.ll"
  test -s "$work/guest.ll"
  for opt in 0 2; do
    "$clang" "-O$opt" -x ir "$work/guest.ll" -x c "$runtime" -std=c11 -pthread -I "$root/src/main/cpp" -o "$work/guest"
    set +e
    timeout 20s "$work/guest" | cat > "$work/stdout"
    status=("${PIPESTATUS[@]}")
    set -e
    test "${status[0]}" = 42
    test "${status[1]}" = 0
    cmp "$work/expected" "$work/stdout"
    printf 'GUEST_PASS|%s|O%s|%s\n' "$name" "$opt" "$blob"
    sha256sum "$work/guest.ll" "$work/guest"
  done
done
for receiver in stdio process; do
  endpoint=out
  if [[ "$receiver" == process ]]; then endpoint=stdout; fi
  printf 'fnc main(): int { let int %s = 0; %s.%s.write("X"); return 42; }\n' "$receiver" "$receiver" "$endpoint" > "$work/shadow.atto"
  if "$compiler" "$work/shadow.atto" > "$work/rejected.ll" 2> "$work/rejected.log"; then
    echo "shadowed endpoint admitted" >&2; exit 1
  fi
  test ! -s "$work/rejected.ll"
  grep -F E_STDOUT_SHADOW "$work/rejected.log"
done
for call in 'process.out.write("X")' 'stdio.err.write("X")' 'process.stderr.write("X")' 'stdio.out.write("X","Y")' 'process.stdout.write("X","Y")'; do
  printf 'fnc main(): int { %s; return 42; }\n' "$call" > "$work/reject.atto"
  if "$compiler" "$work/reject.atto" > "$work/rejected.ll" 2> "$work/rejected.log"; then
    echo "invalid endpoint admitted: $call" >&2; exit 1
  fi
  test ! -s "$work/rejected.ll"
  grep -E 'unsupported native|takes exactly one argument' "$work/rejected.log"
done
