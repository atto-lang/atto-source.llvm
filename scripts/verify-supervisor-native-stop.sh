#!/usr/bin/env bash
# Source-repository verification of the production native STOP CONTROL lane.
# Never interprets a zero-step Actions failure as compiler-test evidence.
# Invoke from the exact source checkout:
#   bash scripts/verify-supervisor-native-stop.sh "$(git rev-parse HEAD)"
set -euo pipefail

if [[ "${1:-}" =~ ^[a-f0-9]{40}$ ]]; then
  expected="$1"
else
  echo "SUPERVISOR_NATIVE_STOP_DENIED: exact 40-hex commit argument required" >&2
  exit 2
fi
for tool in git clang++ g++; do
  if ! command -v "$tool" >/dev/null; then
    echo "SUPERVISOR_NATIVE_STOP_DENIED: missing $tool" >&2
    exit 2
  fi
done
cd "$(git rev-parse --show-toplevel)"
actual="$(git rev-parse --verify HEAD)"
if [[ "$actual" != "$expected" ]]; then
  echo "SUPERVISOR_NATIVE_STOP_DENIED: source does not match exact expected head" >&2
  exit 2
fi
if ! git diff --quiet HEAD -- || ! git diff --cached --quiet; then
  echo "SUPERVISOR_NATIVE_STOP_DENIED: tracked source is dirty" >&2
  exit 2
fi

umask 077
build="$(mktemp -d)"
trap 'rm -rf -- "$build"' EXIT
source_file="tests/supervisor_control_turn_test.cpp"
includes="-Isrc/main/cpp"

# Compile *production* headers, execute native TURN/STOP receipt tests.
clang++ -std=c++17 -O1 -g -Wall -Wextra -Wpedantic -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer -pthread \
  "$includes" "$source_file" -o "$build/stop-asan-ubsan"
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
  "$build/stop-asan-ubsan"

g++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror -pthread \
  "$includes" "$source_file" -o "$build/stop-gcc-o2"
"$build/stop-gcc-o2"

# The two actual executable invocations must have completed successfully.
echo "SUPERVISOR_NATIVE_STOP_EXACT_HEAD=PASS sha=$actual clang_asan_ubsan=PASS gcc_o2=PASS"
