#!/usr/bin/env sh
# Standalone C++17 host actor pipeline routing checks, no LLVM toolchain needed.
# This is NOT evidence of compiled source self.forward_* guest lowering.
set -eu
cd "$(dirname "$0")/.."

for compiler in g++ clang++; do
  command -v "$compiler" >/dev/null 2>&1 || {
    echo "actor pipeline host gate requires both g++ and clang++" >&2; exit 1;
  }
done

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT HUP INT TERM

for compiler in g++ clang++; do
  for opt in 0 2; do
    for test in native_actor_pipe_graph_contract_test native_actor_pipeline_routing_test; do
      "$compiler" -std=c++17 "-O$opt" -g -Wall -Wextra -Wpedantic -Werror \
        -pthread -Isrc/main/cpp "tests/$test.cpp" -o "$work/$test-$compiler-O$opt"
      "$work/$test-$compiler-O$opt"
      echo "ORES_ACTOR_PIPELINE_HOST_PASS compiler=$compiler opt=$opt test=$test"
    done
  done
done

if [ "${ORES_ACTOR_PIPELINE_SANITIZE:-1}" = 1 ]; then
  for compiler in g++ clang++; do
    for test in native_actor_pipe_graph_contract_test native_actor_pipeline_routing_test; do
      "$compiler" -std=c++17 -O1 -g -Wall -Wextra -Wpedantic -Werror \
        -fsanitize=address,undefined -fno-omit-frame-pointer \
        -pthread -Isrc/main/cpp "tests/$test.cpp" -o "$work/$test-$compiler-sanitize"
      ASAN_OPTIONS=detect_leaks=1 "$work/$test-$compiler-sanitize"
      echo "ORES_ACTOR_PIPELINE_SANITIZER_PASS compiler=$compiler test=$test"
    done
  done
fi

echo "ORES_ACTOR_PIPELINE_HOST_MATRIX_PASS"
