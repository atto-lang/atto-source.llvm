#!/usr/bin/env bash
set -euo pipefail

: "${EXPECTED_SOURCE_TREE_SHA:?Set EXPECTED_SOURCE_TREE_SHA to the exact private source Git tree SHA}"
if [[ ! ${EXPECTED_SOURCE_TREE_SHA} =~ ^[0-9a-f]{40}$ ]]; then
  echo "Invalid source Git tree SHA" >&2
  exit 2
fi
actual="$(git rev-parse 'HEAD^{tree}')"
if [[ "${actual}" != "${EXPECTED_SOURCE_TREE_SHA}" ]]; then
  echo "MISMATCH: checked-out Git tree does not match source PR" >&2
  exit 2
fi
git diff --quiet HEAD -- || { echo "Tracked files modified before test" >&2; exit 2; }
if [[ -n "$(git ls-files --others --exclude-standard)" ]]; then
  echo "Untracked checkout files present before exact-tree test; refusing contaminated inputs" >&2
  exit 2
fi

# Host-only analyzer: this does not exercise instrumented LLVM guest programs.
compiler="${CXX:-c++}"
scratch="$(mktemp -d)"
trap 'rm -rf -- "${scratch}"' EXIT
"${compiler}" -std=c++17 -Wall -Wextra -Wpedantic -Werror -UNDEBUG -pthread \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -Isrc/main/cpp tests/concurrency_race_analyzer_test.cpp \
    -o "${scratch}/oreslang-concurrency-race-test"
"${scratch}/oreslang-concurrency-race-test"
if [[ "${RUN_FULL_SUITE:-0}" == "1" ]]; then
  cmake -S . -B "${scratch}/build" -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
  cmake --build "${scratch}/build" --parallel 2
  ctest --test-dir "${scratch}/build" --output-on-failure
fi
git diff --quiet HEAD -- || { echo "Tracked files modified during test" >&2; exit 2; }
echo "ORES_CONCURRENCY_CI|backend=llvm|source_tree=${actual}|result=PASS|scope=host-analyzer-asan-ubsan"
