#!/usr/bin/env sh
# Host-only OpenSSL AEAD verification. This does not compile an Atto guest.
set -eu
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
CXX="${CXX:-c++}"
"$CXX" -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror -UNDEBUG \
  -I"$ROOT/src/main/cpp" \
  "$ROOT/tests/crypto_aead_host_v1_test.cpp" \
  "$ROOT/src/main/cpp/oreslang/runtime/CryptoAeadHostV1.cpp" \
  -lcrypto -pthread -o "$TMP/crypto_aead_host_v1"
"$TMP/crypto_aead_host_v1"
