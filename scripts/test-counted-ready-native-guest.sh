#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"
compiler="$(realpath "$1")"
corpus="$(realpath "$2")"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
python3 - "$corpus" <<'PY'
import json,sys,pathlib,hashlib
root=pathlib.Path(sys.argv[1])
cases=[c for c in json.loads((root/'manifest.json').read_text())['cases'] if c['id'].startswith(('counted-','reject-counted-'))]
assert len(cases)==17
for c in cases:
    raw=(root/c['path']).read_bytes()
    assert hashlib.sha1(b'blob '+str(len(raw)).encode()+b'\0'+raw).hexdigest()==c['gitBlobSha1']
PY
for name in counted-distinct-alias-projection counted-three counted-zero counted-one counted-alias counted-dynamic counted-dynamic-zero counted-mixed-future-alias counted-multi-channel-fair counted-mixed-zero; do
  "$compiler" --counted-ready-v1 "$corpus/$name.atto" > "$work/guest.cpp"
  clang++ -std=c++17 -O1 -pthread -I src/main/cpp -S -emit-llvm "$work/guest.cpp" -o "$work/guest.ll"
  clang++ -O1 -pthread "$work/guest.ll" -o "$work/guest"
  test "$("$work/guest")" = 42
  echo "GUEST_PASS $name"
done
for name in reject-counted-negative reject-counted-bool reject-counted-scalar reject-counted-default reject-counted-write reject-counted-dynamic-bool reject-counted-dynamic-unknown; do
  if "$compiler" --counted-ready-v1 "$corpus/$name.atto" > "$work/denial" 2> "$work/err"; then exit 1; fi
  test ! -s "$work/denial"
  grep -F E_COUNTED_READY_PROFILE "$work/err"
done
# Additional actual generated guest error controls; no constant folding oracle.
for case in invalid-count overflow-before-await empty-read would-suspend capacity; do
  case "$case" in
    invalid-count) body='val int count = 17; val Array[int] b = await select<int>(count) { when readch a: int n -> { return n; } }; return b[0];'; expected=SelectCountInvalid;;
    overflow-before-await) body='writech a, 42; val Future<Array<int>> pending = select<int>(1) { when readch a: int n -> { return n; } }; val int x = 9223372036854775807 + 1; val Array[int] b = await pending; return b[0];'; expected=E_I64_OVERFLOW;;
    empty-read) body='val Array[int] b = await select<int>(0) { when readch a: int n -> { return n; } }; return readch a;'; expected=E_READY_ONLY;;
    would-suspend) body='val Array[int] b = await select<int>(1) { when readch a: int n -> { return n; } }; return b[0];'; expected=E_READY_ONLY;;
    capacity) body='writech a, 42; writech a, 43; val Array[int] b = await select<int>(1) { when readch a: int n -> { return n; } }; return b[0];'; expected=E_READY_ONLY;;
  esac
  printf 'suspends fnc main(): int { val Channel<int> a = Channel.new<int>(1); %s }\n' "$body" > "$work/$case.atto"
  "$compiler" --counted-ready-v1 "$work/$case.atto" > "$work/guest.cpp"
  clang++ -std=c++17 -O1 -pthread -fsanitize=address,undefined -fno-sanitize-recover=all -I src/main/cpp "$work/guest.cpp" -o "$work/guest"
  status=0
  ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "$work/guest" > "$work/out" 2> "$work/err" || status=$?
  test "$status" = 2
  test ! -s "$work/out"
  grep -F "$expected" "$work/err"
  ! grep -E 'AddressSanitizer|runtime error:|LeakSanitizer' "$work/err"
  echo "NEGATIVE_RUNTIME_PASS $case"
done
# Canonical raw Future retains concrete scalar/array types and affine ownership.
prefix='suspends fnc main(): int { const Channel<int> a = Channel.new<int>(1); writech a, 20; '
body='const Future f = select { default: -> { return 20; } }; const Future batch = select(1) { when readch a: int n -> { return n; } }; const Array[int] values = await batch; const int extra = await select<int> { default: -> { return 2; } }; return (await f) + values[0] + extra; }'
printf '%s%s\n' "$prefix" "$body" > "$work/raw-future.atto"
"$compiler" --counted-ready-v1 "$work/raw-future.atto" > "$work/guest.cpp"
clang++ -std=c++17 -O1 -pthread -I src/main/cpp -S -emit-llvm "$work/guest.cpp" -o "$work/guest.ll"
clang++ -O1 -pthread "$work/guest.ll" -o "$work/guest"
test "$("$work/guest")" = 42
echo GUEST_PASS raw-future
for body in \
  'const Future f = 42; return 42; }' \
  'const Future<int> f = select(1) { when readch a: int n -> { return n; } }; return 42; }' \
  'const Future f = select { default: -> { return a; } }; return 42; }' \
  'const Future b = select(1) { when readch a: int n -> { return n; } }; return 42; }' \
  'const Future b = select(1) { when readch a: int n -> { return n; } }; const int wrong = await b; return wrong; }' \
  'const Future b = select(1) { when readch a: int n -> { return n; } }; const Array[int] first = await b; const Array[int] second = await b; return first[0]; }'; do
  printf '%s%s\n' "$prefix" "$body" > "$work/raw-negative.atto"
  if "$compiler" --counted-ready-v1 "$work/raw-negative.atto" > "$work/denial" 2> "$work/err"; then exit 1; fi
  test ! -s "$work/denial"
  grep -F E_COUNTED_READY_PROFILE "$work/err"
done
echo LLVM_COUNTED_READY_NATIVE_GUEST_PASS
