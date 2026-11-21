#!/usr/bin/env bash
# Exact native-machine LLVM AOT validation (not a network interoperability test).
set -euo pipefail
cd "$(dirname "$0")/.."
export CARGO_NET_OFFLINE=true
test "$(git hash-object frontend/src/check.rs)" = "fc8806e97d94f408fd145f48b73ab33324dae2fa"
test "$(git hash-object frontend/src/parser.rs)" = "5c38eae45452aee597885a38af8903b54f1f4e37"
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

# Output atomicity and source preservation are security gates, not conveniences.
cat > "$work/alias-source.atto" <<'ATTO'
pub fnc main(): int { return 42; }
ATTO
cp "$work/alias-source.atto" "$work/source-baseline"
if target/release/atto-llvm "$work/alias-source.atto" -o "$work/alias-source.atto" --emit-llvm >"$work/alias.log" 2>&1; then
  echo "FAIL: --emit-llvm overwrote source" >&2; exit 1
fi
grep -q E_OUTPUT_SOURCE "$work/alias.log"
cmp "$work/alias-source.atto" "$work/source-baseline"
if target/release/atto-llvm "$work/alias-source.atto" -o "$work/alias-source.atto" >"$work/alias.log" 2>&1; then
  echo "FAIL: native output overwrote source" >&2; exit 1
fi
grep -q E_OUTPUT_SOURCE "$work/alias.log"
cmp "$work/alias-source.atto" "$work/source-baseline"

ln -s "$work/alias-source.atto" "$work/source-symlink.ll"
ln "$work/alias-source.atto" "$work/source-hardlink.ll"
for alias in source-symlink.ll source-hardlink.ll; do
  if target/release/atto-llvm "$work/alias-source.atto" -o "$work/$alias" --emit-llvm >"$work/alias.log" 2>&1; then
    echo "FAIL: emitted IR through source alias $alias" >&2; exit 1
  fi
  grep -q E_OUTPUT_SOURCE "$work/alias.log"
  cmp "$work/alias-source.atto" "$work/source-baseline"
done
echo "PASS output path alias and hardlink/symlink protection"

printf 'PREVIOUS_KNOWN_GOOD_BINARY\n' > "$work/previous.exe"
cp "$work/previous.exe" "$work/previous-baseline"
if target/release/atto-llvm "$work/unknown.atto" -o "$work/previous.exe" >"$work/negative.log" 2>&1; then
  echo "FAIL: malformed input accepted with prior artifact" >&2; exit 1
fi
grep -q E_UNBOUND "$work/negative.log"
cmp "$work/previous.exe" "$work/previous-baseline"

mkdir "$work/fakebin"
cat > "$work/fakebin/clang" <<'SH'
#!/bin/sh
exit 27
SH
chmod +x "$work/fakebin/clang"
if PATH="$work/fakebin:$PATH" target/release/atto-llvm "$work/alias-source.atto" -o "$work/previous.exe" >"$work/clang.log" 2>&1; then
  echo "FAIL: failed clang returned success" >&2; exit 1
fi
grep -q E_CLANG "$work/clang.log"
cmp "$work/previous.exe" "$work/previous-baseline"

# An adversarial compiler executable can falsely return 0. A blank or invalid
# output must never be published over a previously valid artifact.
cat > "$work/fakebin/clang" <<'SH'
#!/bin/sh
cat >/dev/null
exit 0
SH
chmod +x "$work/fakebin/clang"
if PATH="$work/fakebin:$PATH" target/release/atto-llvm "$work/alias-source.atto" -o "$work/previous.exe" >"$work/clang.log" 2>&1; then
  echo "FAIL: fake successful clang published empty artifact" >&2; exit 1
fi
grep -q E_CLANG_ARTIFACT "$work/clang.log"
cmp "$work/previous.exe" "$work/previous-baseline"

cat > "$work/fakebin/clang" <<'SH'
#!/bin/sh
output=""
while [ "$#" -gt 0 ]; do
  if [ "$1" = "-o" ]; then
    shift
    output="$1"
    break
  fi
  shift
done
printf 'not-an-ELF-binary' > "$output"
cat >/dev/null
exit 0
SH
chmod +x "$work/fakebin/clang"
if PATH="$work/fakebin:$PATH" target/release/atto-llvm "$work/alias-source.atto" -o "$work/previous.exe" >"$work/clang.log" 2>&1; then
  echo "FAIL: fake successful clang published invalid artifact" >&2; exit 1
fi
grep -q E_CLANG_ARTIFACT "$work/clang.log"
cmp "$work/previous.exe" "$work/previous-baseline"
echo "PASS reject zero-exit compiler with missing or invalid ELF"

# A successful publish must replace the old artifact atomically, whether
# emitting LLVM IR or invoking clang. No staged temporary files may remain.
target/release/atto-llvm "$work/alias-source.atto" -o "$work/previous.exe" --emit-llvm
grep -q 'define i32 @main' "$work/previous.exe"
target/release/atto-llvm "$work/alias-source.atto" -o "$work/previous.exe"
set +e
timeout 10s "$work/previous.exe"
publish_status=$?
set -e
test "$publish_status" = 42
if find "$work" -type f -name '*.atto-stage-*.tmp' | grep -q .; then
  echo "FAIL: temporary compiler artifacts leaked" >&2; exit 1
fi
echo "PASS atomic output, failure preservation, and cleanup"

# The head's guard parser is SHA-pinned above. Do not mistake the historical
# 1255-case scalar inventory for evidence that the two guard block spellings
# execute as actual native machine code. Run the independent guest gate.
bash ci/test_guard_dual_form_native.sh

echo "PASS LLVM_NATIVE_AOT_SCALAR_GUEST cases=$total toolchain=clang no_guest_vm=1 zero_gc_scalar=1"
