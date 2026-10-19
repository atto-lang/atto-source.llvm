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

# Host-only analyzer: this does not exercise instrumented LLVM guest programs.
compiler="${CXX:-c++}"
"${compiler}" -std=c++17 -Wall -Wextra -Wpedantic -Werror -UNDEBUG -pthread \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -Isrc/main/cpp tests/concurrency_race_analyzer_test.cpp \
    -o /tmp/oreslang-concurrency-race-test
/tmp/oreslang-concurrency-race-test
git diff --quiet HEAD -- || { echo "Tracked files modified during test" >&2; exit 2; }
echo "ORES_CONCURRENCY_CI|backend=llvm|source_tree=${actual}|result=PASS|scope=host-analyzer-asan-ubsan"
