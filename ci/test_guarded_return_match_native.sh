#!/usr/bin/env bash
# Native scalar Atto -> LLVM IR -> Clang -> executable conformance.
# The stdout variants are pinned identically, but host println is still
# E_UNSUPPORTED_NATIVE_RUNTIME; never count these as full source parity.
set -euo pipefail
cd "$(dirname "$0")/.."
export CARGO_NET_OFFLINE=true
test "$(git hash-object frontend/src/parser.rs)" = "629a596d9a0015ceca107222f4965875c9cb5111"
test "$(git hash-object frontend/src/lexer.rs)" = "f48dc2b2c9759dcebec77d30daecd4fc8e412944"
test "$(git hash-object tests/match-return-forms-v1/pass/match-guard-block-return.atto)" = "603df679e1e9b7415cb8b798d4a2143545dd27b8"
test "$(git hash-object tests/match-return-forms-v1/pass/match-guard-block-caret.atto)" = "0b2addfef29cd76d60d90bf081f4d326ea2d70df"
test "$(git hash-object tests/match-return-forms-v1/fail/match-guard-no-fallback.atto)" = "23f8597125af43b3cab294ba8dc9b110d9983ef5"
test "$(git hash-object tests/match-return-forms-v1/fail/match-guard-block-fallthrough.atto)" = "a8ec2c699c4a521508bee85f55a49888064c6948"
cargo test --offline
cargo build --offline --release --bin atto-llvm
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
for form in return caret; do
  input="tests/match-return-forms-v1/native-${form}-scalar.atto"
  target/release/atto-llvm "$input" -o "$work/${form}.ll" --emit-llvm
  target/release/atto-llvm "$input" -o "$work/${form}-again.ll" --emit-llvm
  cmp "$work/${form}.ll" "$work/${form}-again.ll"
  clang -x ir -O2 -fno-lto -o "$work/${form}.exe" "$work/${form}.ll"
  set +e
  timeout 15s "$work/${form}.exe"
  status=$?
  set -e
  test "$status" = 4 || { echo "FAIL native ${form}: expected 4, received $status" >&2; exit 1; }
  echo "PASS native-${form}-scalar: compiled guest exit=4"
done

for item in match-guard-no-fallback:E_MATCH match-guard-block-fallthrough:E_RETURN; do
  name="${item%%:*}"
  expected="${item#*:}"
  artifact="$work/${name}.exe"
  test ! -e "$artifact"
  if target/release/atto-llvm "tests/match-return-forms-v1/fail/${name}.atto" -o "$artifact" >"$work/${name}.log" 2>&1; then
    echo "FAIL: negative fixture compiled: $name" >&2; exit 1
  fi
  grep -q "$expected" "$work/${name}.log"
  test ! -e "$artifact"
  echo "PASS rejected ${name} with ${expected}, no artifact"
done

# The exact user programs contain print I/O. This native scalar compiler
# must fail closed until stdout support is implemented; not equivalent PASS.
for form in return caret; do
  case "$form" in
    return) file=match-guard-block-return ;;
    caret) file=match-guard-block-caret ;;
  esac
  artifact="$work/${form}-stdout.exe"
  if target/release/atto-llvm "tests/match-return-forms-v1/pass/${file}.atto" -o "$artifact" >"$work/${form}-stdout.log" 2>&1; then
    echo "FAIL: unimplemented stdout target compiled without a supported runtime" >&2; exit 1
  fi
  grep -q E_UNSUPPORTED_NATIVE_RUNTIME "$work/${form}-stdout.log"
  test ! -e "$artifact"
  echo "UNSUPPORTED exact print fixture: native stdout contract missing"
done
echo "MATCH_RETURN_SCALAR_NATIVE_AOT_PASS; EXACT_STDOUT_PROGRAMS_UNSUPPORTED"
