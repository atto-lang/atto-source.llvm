#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
cxx="${CXX:-c++}"
"$cxx" -std=c++17 -Wall -Wextra -Wpedantic -Werror -I src/main/cpp \
  tests/native_quic_admission_test.cpp -o "$tmp/native-quic-admission"
"$tmp/native-quic-admission"
