#!/usr/bin/env bash
set -euo pipefail
compiler=$1
clang=$2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
run_case() {
  printf '%s\n' "$1" > "$tmp/case.atto"
  "$compiler" "$tmp/case.atto" > "$tmp/case.ll"
  for opt in O0 O2; do
    "$clang" "-$opt" -Wno-override-module -x ir "$tmp/case.ll" -o "$tmp/guest"
    set +e
    "$tmp/guest"
    rc=$?
    set -e
    test "$rc" = 42
  done
}
reject() {
  printf '%s\n' "pub fnc main(): int { $1 return 0; }" > "$tmp/reject.atto"
  if "$compiler" "$tmp/reject.atto" > "$tmp/reject.ll" 2> "$tmp/reject.err"; then
    echo "invalid aggregate accepted" >&2
    exit 1
  fi
  test ! -s "$tmp/reject.ll"
  grep -q "$2" "$tmp/reject.err"
}
run_case 'pub fnc main(): int { const Record<infer R> r=(a=1,b=true); const Tuple<infer T> t=(true,2); const struct<infer S> s={a:3}; const Record<R> r2=(b=false,a=10); const Tuple<T> t2=(false,12); const struct<S> s2={a:20}; return r2.a+t2[1]+s2.a; }'
run_case 'pub fnc main(): int { const Record r=(a=10); const Tuple t=(true,12); const struct s={a:20}; return r.a+t[1]+s.a; }'
run_case 'pub fnc main(): int { const Record<infer R> r=(a=-1); const Record<R> s=(a=43); return r.a+s.a; }'
reject 'const Record<infer> r=(a=1);' E_TYPE_NAME
reject 'const Record<R> r=(a=1);' E_UNKNOWN_TYPE
reject 'const Record<infer Map> r=(a=1);' E_TYPE_NAME
reject 'const Record<infer R> r=(a=1); const Tuple<infer R> t=(1,2);' E_DUPLICATE_TYPE
reject 'const Record<infer R> r=(a=1); const Record<R> s=(a=true);' E_TYPE_MISMATCH
reject 'const Record<infer R> r=(a=1); const Record<R> s=(a=1,b=2);' E_TYPE_MISMATCH
reject 'const Record<infer R> r=(a=1,b=2); const Record<R> s=(a=1);' E_TYPE_MISMATCH
reject 'const Record<infer R> r=(a=1,a=2);' E_DUPLICATE_FIELD
reject 'const Tuple<infer T> t=(1,true); const Tuple<T> s=(true,1);' E_TYPE_MISMATCH
reject 'const Tuple<infer T> t=(1,true); const Tuple<T> s=(1,);' E_TYPE_MISMATCH
reject 'const struct<infer S> s={a:1}; const Record<S> r=(a=1);' E_AGGREGATE_KIND
reject 'const Record<infer R> r=(a=unknown);' E_TYPE_INFER_AMBIGUOUS
run_case 'pub fnc main(): int { const t=(); return 42; }'
run_case 'pub fnc main(): int { const t=(/* empty */); const Record r=(); const Record s={}; const Tuple u=(); return 42; }'
run_case 'pub fnc main(): int { const Record<infer R> r=(); const Record<R> s={}; return 42; }'
run_case 'pub fnc main(): int { const Tuple<infer T> t=(); const Tuple<T> u=(); return 42; }'
run_case 'pub fnc main(): int { const Tuple t=(42,); return t[0]; }'
run_case 'pub fnc main(): int { const Record r={a:40,b=2}; return r.a+r.b; }'
run_case 'pub fnc main(): int { const Tuple t=(42); return t[0]; }'
run_case 'pub fnc main(): int { const Tuple<infer T> t=(1); const Tuple<T> u=(42,); return u[0]; }'
reject 'const Tuple<infer T> t=(1,2); const Tuple<T> u=(42);' E_TYPE_MISMATCH
reject 'const Record<infer R> r=(); const Tuple<R> t=();' E_AGGREGATE_KIND
reject 'const Tuple<infer T> t=(); const Record<T> r=();' E_AGGREGATE_KIND
reject 'const Record<infer R> r={a:1}; const Record<R> s=();' E_TYPE_MISMATCH
reject 'const Record<infer R> r=(); const Record<R> s={a:1};' E_TYPE_MISMATCH
reject 'const Tuple<infer T> t=(1,2); const Tuple<T> u=();' E_TYPE_MISMATCH
reject 'const struct s={};' E_EMPTY_AGGREGATE
reject 'const t=(); const t=();' 'duplicate aggregate local'
run_case 'pub fnc main(): int { const x=(42); return x; }'
reject 'const Tuple t=(x:42);' E_TYPE_INFER_AMBIGUOUS
reject 'const Record r=(42);' E_TYPE_NAME
echo 'Named aggregate LLVM native guests: 12 x O0/O2, 22 rejection cases'
