#!/usr/bin/env sh
set -eu
cd "$(dirname "$0")/.."
out="$(mktemp "${TMPDIR:-/tmp}/atto-actor-isolation-v1.XXXXXX")"
trap 'rm -f "$out"' EXIT HUP INT TERM
c++ -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror -UNDEBUG -I "src/main/cpp" \
  tests/actor_isolation_access_v1_test.cpp -o "$out"
"$out"
