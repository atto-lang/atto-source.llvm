#!/usr/bin/env bash
# Exact native-machine LLVM AOT validation (not a network interoperability test).
set -euo pipefail
cd "$(dirname "$0")/.."
export CARGO_NET_OFFLINE=true
test "$(git hash-object frontend/src/check.rs)" = "18c9b7e3e9696bfdd56fa5130f52a9a71a796c43"
test "$(git hash-object frontend/src/parser.rs)" = "6231be661be9b7bb40bd9ed69d4cd60ccc5b9609"
test "$(git hash-object frontend/src/lexer.rs)" = "23f8fe950b47627110cf3265b771398bb46213e3"

cargo test --offline
cargo build --offline --release --bin atto-llvm
clang --version | head -n 1
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

total=0
for case in tests/fixtures/1*.atto; do
  name="$(basename "$case" .atto)"
  artifact="$work/$name"
  target/release/atto-llvm "$case" -o "$artifact" --emit-llvm
  target/release/atto-llvm "$case" -o "$work/again.ll" --emit-llvm
  cmp "$artifact" "$work/again.ll"
  clang -x ir -O2 -fno-lto -o "$work/$name.exe" "$artifact"
  readelf -h "$work/$name.exe" | grep -q 'Class:[[:space:]]*ELF64'
  set +e
  timeout 15s "$work/$name.exe"
  status=$?
  set -e
  if [[ "$status" != 42 ]]; then
    echo "FAIL: native Atto $name returned $status, expected 42" >&2
    exit 1
  fi
  sha256sum "$artifact" "$work/$name.exe"
  case "$name" in
    167-*) total=$((total+308));;
    168-*) total=$((total+435));;
    169-*|170-*) total=$((total+256));;
    *) echo "unexpected fixture: $name" >&2; exit 1;;
  esac
  echo "PASS native-AOT $name"
done
test "$total" = "1255"

# Exercise the CLI's direct -o native-executable path, not merely emitted IR.
target/release/atto-llvm tests/fixtures/167-quic-varint-rfc9000.atto -o "$work/direct-native.exe"
readelf -h "$work/direct-native.exe" | grep -q 'Class:[[:space:]]*ELF64'
set +e
timeout 15s "$work/direct-native.exe"
direct_status=$?
set -e
test "$direct_status" = "42"
echo "PASS direct atto-llvm -> clang -> ELF executable"


cat > "$work/unknown.atto" <<'ATTO'
pub fnc main(): int { return missing_symbol; }
ATTO
if target/release/atto-llvm "$work/unknown.atto" -o "$work/unknown.exe" >"$work/negative.log" 2>&1; then
  echo "FAIL: unbound Atto source compiled" >&2; exit 1
fi
grep -q E_UNBOUND "$work/negative.log"
test ! -e "$work/unknown.exe"

cat > "$work/unsafe.atto" <<'ATTO'
pub fnc main(): int { stdio.println(1); return 42; }
ATTO
if target/release/atto-llvm "$work/unsafe.atto" -o "$work/unsafe.exe" >"$work/unsupported.log" 2>&1; then
  echo "FAIL: unsupported host print compiled" >&2; exit 1
fi
grep -q E_UNSUPPORTED_NATIVE_RUNTIME "$work/unsupported.log"
test ! -e "$work/unsafe.exe"

cat > "$work/overflow.atto" <<'ATTO'
pub fnc main(): int { return 9223372036854775807 + 1; }
ATTO
target/release/atto-llvm "$work/overflow.atto" -o "$work/overflow.ll" --emit-llvm
grep -q llvm.sadd.with.overflow "$work/overflow.ll"
clang -x ir -O2 -o "$work/overflow.exe" "$work/overflow.ll"
set +e
(ulimit -c 0; timeout 10s "$work/overflow.exe") >/dev/null 2>&1
overflow_status=$?
set -e
if [[ "$overflow_status" == 0 || "$overflow_status" == 42 || "$overflow_status" == 124 ]]; then
  echo "FAIL: i64 overflow did not trap (status=$overflow_status)" >&2; exit 1
fi
echo "PASS checked native overflow traps with status=$overflow_status"

# Negative arithmetic bounds must trap; never wrap like C unsigned math.
for expr in '(-9223372036854775807 - 2)' '(9223372036854775807 * 2)' '(-(-9223372036854775807 - 1))'; do
  printf 'pub fnc main(): int { return %s; }\n' "$expr" > "$work/overflow.atto"
  target/release/atto-llvm "$work/overflow.atto" -o "$work/overflow.exe"
  set +e
  (ulimit -c 0; timeout 10s "$work/overflow.exe") >/dev/null 2>&1
  overflow_status=$?
  set -e
  if [[ "$overflow_status" == 0 || "$overflow_status" == 42 || "$overflow_status" == 124 ]]; then
    echo "FAIL: checked arithmetic unexpectedly continued for $expr" >&2
    exit 1
  fi
done
echo "PASS checked native sub/mul/neg overflow traps"


# Recompile two guests with both sanitizers. These are pure scalar workloads:
# absence of errors does not validate future actors, heaps, TLS, QUIC or network I/O.
for name in 167-quic-varint-rfc9000 168-http3-frame-rfc9114; do
  clang -x ir -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
    -o "$work/sanitized.exe" "$work/$name"
  set +e
  ASAN_OPTIONS=detect_leaks=1 timeout 30s "$work/sanitized.exe"
  status=$?
  set -e
  if [[ "$status" != 42 ]]; then
    echo "FAIL: ASan/UBSan $name returned $status" >&2; exit 1
  fi
  echo "PASS ASan/UBSan native-AOT $name"
done
echo "PASS LLVM_NATIVE_AOT_SCALAR_GUEST cases=$total toolchain=clang no_guest_vm=1 zero_gc_scalar=1"
