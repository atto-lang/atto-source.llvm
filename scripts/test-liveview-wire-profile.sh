#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
tmp="$(mktemp -d)"
trap 'rm -rf -- "$tmp"' EXIT
compiler="${CXX:-g++}"
"$compiler" -std=c++20 -O1 -g -Wall -Wextra -Werror -pedantic \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  "$root/tests/liveview_wire_profile_test.cpp" -o "$tmp/liveview-wire-test"
ASAN_OPTIONS=detect_leaks=1 "$tmp/liveview-wire-test"
echo 'HOST_ONLY: no native .ores guest, socket actor, AOT or five-backend parity'
