#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
: "${ORESLANG_LLVMC:?absolute exact-head native compiler required}"
test -x "$ORESLANG_LLVMC"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cd "$work"
fib() { printf 'pub fnc main(): int { return %s |> fnc fib(int v): int { ^ match v over on 0 -> 0, on 1 -> 1, on _ -> fib(v-2)+fib(v-1) end }; }\n' "$1"; }
count() { printf 'pub fnc main(): int { return %s |> fnc count(int v): int { return match v over on 0 -> 0, on _ -> count(v-1)+1 end; }; }\n' "$1"; }
for spec in 'fib0:0:0' 'fib1:0:1' 'fib10:0:55' 'fib20:0:6765' 'count1024:0:1024' 'count1025:6:0' 'overflow:1:0' 'minimum:0:-9223372036854775807'; do
  IFS=: read -r id status value <<<"$spec"
  case "$id" in
    fib*) fib "${id#fib}" > input.atto ;;
    count*) count "${id#count}" > input.atto ;;
    overflow) echo 'pub fnc main(): int {return 9223372036854775807 |> fnc f(int v): int {return v+1;};}' >input.atto ;;
    minimum) echo 'pub fnc main(): int {return -9223372036854775808 |> fnc f(int v): int {return v+1;};}' >input.atto ;;
  esac
  timeout 30s "$ORESLANG_LLVMC" --named-recursive-scalar-v1 input.atto > guest.ll 2>err
  test ! -s err
  grep -q '^define i32 @atto_named_main' guest.ll
  if grep -q '^define i32 @atto_named_scalar' guest.ll; then exit 1; fi
  for opt in 0 2; do
    clang -Wno-override-module -O"$opt" -c guest.ll -o guest.o
    clang -Wall -Wextra -Werror "$root/tests/named-recursive-native-ir/source-host.c" guest.o -o guest
    timeout 5s ./guest "$status" "$value"
    echo "PASS source $id O$opt"
  done
done
cat > input.atto <<'EOF'
pub fnc main(): int {
    return 55 |> fnc fib(int v): int {
        ^ match v over
            on 0 -> 0,
            on 1 -> 1,
            on _ -> fib(v - 2) + fib(v - 1)
        end
    };
}
EOF
test "$(git hash-object input.atto)" = ab60c8c4ad9726a526a4b0aa9ba76c0acca50b9d
timeout 30s "$ORESLANG_LLVMC" --named-recursive-scalar-v1 input.atto > guest.ll
clang -Wno-override-module -O2 -c guest.ll -o exact55.o
test -s exact55.o
echo 'COMPILE_ONLY exact fib55'
while IFS='|' read -r code source; do
  printf '%s\n' "$source" > input.atto
  rc=0
  timeout 30s "$ORESLANG_LLVMC" --named-recursive-scalar-v1 input.atto >out 2>err || rc=$?
  test "$rc" -eq 2
  test ! -s out
  grep -Fq "$code" err
  echo "PASS rejection $code"
done <<'EOF'
E_NAMED_STAGE_CLOSED_INT|pub fnc main(): int {return 1 |> fnc f(int v): int {return v+x;};}
E_NAMED_STAGE_NAME|pub fnc main(): int {return 1 |> fnc f(int f): int {return f;};}
E_NAMED_STAGE_GRAPH|pub fnc main(): int {return 1 |> fnc f(int v): int {^ match v over on 0 -> 0 end};}
E_NAMED_STAGE_GRAPH|pub fnc main(): int {return 1 |> fnc f(int v): int {^ match v over on 0 -> 0, on 0 -> 1, on _ -> 2 end};}
E_NAMED_STAGE_PROFILE|pub fnc main(): int {return 1 |> fnc f(int v): int {return v;};} fnc x(): int {return 42;}
E_NAMED_STAGE_PROFILE|pub fnc main(): int {return 1 |> fnc f(int v): int {return "v";};}
EOF
