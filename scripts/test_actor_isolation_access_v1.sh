#!/usr/bin/env sh
set -eu
cd "$(dirname "$0")/.."
out="$(mktemp "${TMPDIR:-/tmp}/atto-actor-isolation-v1.XXXXXX")"
trap 'rm -f "$out"' EXIT HUP INT TERM
compiler="${CXX:-c++}"
for test_source in \
  tests/actor_isolation_access_v1_test.cpp \
  tests/actor_isolation_access_scoped_test.cpp
do
  "$compiler" -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror -UNDEBUG -pthread -I "src/main/cpp" \
    "$test_source" -o "$out"
  "$out"
done
printf '%s\n' 'ACTOR_ISOLATION_HOST_AND_SCOPED_TESTS_EXECUTED'
