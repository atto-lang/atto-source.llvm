#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
cxx="${CXX:-c++}"
"$cxx" -std=c++17 -Wall -Wextra -Wpedantic -Werror -UNDEBUG -I src/main/cpp \
  tests/native_quic_admission_test.cpp -o "$tmp/native-quic-admission"
"$tmp/native-quic-admission"
"$cxx" -std=c++17 -Wall -Wextra -Wpedantic -Werror -UNDEBUG -I src/main/cpp \
  tests/native_quic_lease_ledger_test.cpp -o "$tmp/native-quic-lease-ledger"
"$tmp/native-quic-lease-ledger"
"$cxx" -std=c++17 -Wall -Wextra -Wpedantic -Werror -UNDEBUG -pthread \
  -fsanitize=address,undefined -fno-omit-frame-pointer -I src/main/cpp \
  tests/native_quic_loopback_udp_test.cpp -o "$tmp/native-quic-loopback-udp"
"$tmp/native-quic-loopback-udp"
