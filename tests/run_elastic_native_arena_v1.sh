#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

# This is a native HOST-ONLY test, never a compiled AttoLang actor receipt.
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

for cc in clang gcc; do
  command -v "$cc" >/dev/null || { echo "Missing required compiler: $cc" >&2; exit 1; }
  for optimization in 1 2; do
    "$cc" -std=c11 -Wall -Wextra -Wpedantic -Werror "-O$optimization" \
      -g -fsanitize=address,undefined -fno-omit-frame-pointer \
      -DORES_ELASTIC_TESTING -I src/main/cpp \
      src/main/cpp/oreslang/runtime/ActorElasticNativeArenaV1.c \
      tests/actor_elastic_native_arena_v1_test.c \
      -o "$tmp/elastic-native-$cc-O$optimization"
    ASAN_OPTIONS=detect_leaks=1 "$tmp/elastic-native-$cc-O$optimization"
  done
done
echo "PASS elastic-native-v1 HOST_ONLY (GCC/Clang O1/O2 ASan+UBSan)"
