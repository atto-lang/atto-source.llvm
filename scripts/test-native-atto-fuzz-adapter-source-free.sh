#!/usr/bin/env bash
# Deliberately source-free ABI/link/diagnostic canary. No AttoLang compiler,
# no actor scheduler, no claim of runtime guest sanitizer coverage.
set -euo pipefail
(( $# == 1 )) ||
  { echo 'usage: test-native-atto-fuzz-adapter-source-free.sh CLANG' >&2; exit 64; }
clang="$1"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
adapter="$root/tests/native_atto_fuzz_stdin_adapter.c"
[[ -f "$adapter" ]] || { echo 'native fuzz adapter source missing' >&2; exit 69; }
command -v "$clang" >/dev/null || { echo 'Clang must be installed' >&2; exit 69; }
work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT
cat > "$work/good.c" <<'GUEST'
#include <stdint.h>
int64_t fuzz_byte(int64_t value) { return value + 1; }
GUEST
cat > "$work/mutant.c" <<'GUEST'
#include <stdint.h>
int64_t fuzz_byte(int64_t value) { return value + 2; }
GUEST
for guest in good mutant; do
  "$clang" -S -emit-llvm -O0 "$work/$guest.c" -o "$work/$guest.ll"
  "$clang" -O1 -g -fno-omit-frame-pointer -Wno-override-module \
    -x ir "$work/$guest.ll" -x c -std=c11 \
    -Wall -Wextra -Wpedantic -Werror "$adapter" -o "$work/$guest"
done
"$clang" -O1 -g -fno-omit-frame-pointer -Wno-override-module \
  -fsanitize=address,undefined -fno-sanitize-recover=all \
  -x ir "$work/good.ll" -x c -std=c11 \
  -Wall -Wextra -Wpedantic -Werror "$adapter" -o "$work/sanitized"
printf '\x00\x01\x7f\xff' | "$work/good" >"$work/out" 2>"$work/err"
[[ ! -s "$work/out" && ! -s "$work/err" ]] ||
  { echo 'source-free adapter valid-byte control failed' >&2; exit 1; }
"$work/good" </dev/null >"$work/out" 2>"$work/err"
[[ ! -s "$work/out" && ! -s "$work/err" ]] ||
  { echo 'source-free adapter empty input control failed' >&2; exit 1; }
head -c 65536 /dev/zero | "$work/good" >"$work/out" 2>"$work/err"
[[ ! -s "$work/out" && ! -s "$work/err" ]] ||
  { echo 'source-free adapter exact byte bound failed' >&2; exit 1; }
set +e
head -c 65537 /dev/zero | "$work/good" >"$work/out" 2>"$work/err"
over=$?
set -e
[[ "$over" == 2 && ! -s "$work/out" ]] &&
  grep -Fq 'ATTO_LLVM_FUZZ_INCONCLUSIVE input budget exceeded' "$work/err" ||
  { echo 'source-free adapter overflow must be inconclusive exit 2' >&2; exit 1; }
set +e
printf '\x00' | "$work/mutant" >"$work/out" 2>"$work/err"
wrong=$?
set -e
[[ "$wrong" == 77 && ! -s "$work/out" ]] &&
  grep -Fq 'ATTO_LLVM_FUZZ_MISMATCH position=0 input=0 expected=1 got=2' "$work/err" ||
  { echo 'source-free mutated IR mismatch not detected' >&2; exit 1; }
printf '\x00\xff' | "$work/sanitized" >"$work/out" 2>"$work/err"
[[ ! -s "$work/out" && ! -s "$work/err" ]] ||
  { echo 'source-free sanitized adapter unexpectedly failed' >&2; exit 1; }
echo 'ATTO_LLVM_STDIN_ADAPTER_SOURCE_FREE_PASS controls=6'
