#!/usr/bin/env bash
# Execute ONLY real in-repo C host preflight tests; no source copying, fetches,
# proprietary cross-org mirror, compiler/guest or native f32 GEMM claim.
set -Eeuo pipefail
cd "$(dirname "$0")/.."
tmp="$(mktemp -d)"
trap 'rm -rf -- "$tmp"' EXIT
for cc in gcc clang; do
  if ! command -v "$cc" >/dev/null 2>&1; then
    echo "F32_NATIVE_ADMISSION_REJECTED: missing $cc" >&2
    exit 2
  fi
  for opt in O1 O2; do
    "$cc" -std=c11 -Wall -Wextra -Werror -Wpedantic \
      "-$opt" -g -pthread -fsanitize=address,undefined \
      -fno-omit-frame-pointer -Isrc/main/cpp \
      tests/native_f32_actor_host_admission_v1_test.c \
      -o "$tmp/f32-actor-host"
    ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 \
      UBSAN_OPTIONS=halt_on_error=1 "$tmp/f32-actor-host"
    "$cc" -std=c11 -Wall -Wextra -Werror -Wpedantic \
      "-$opt" -g -pthread -fsanitize=address,undefined \
      -fno-omit-frame-pointer -Isrc/main/cpp \
      tests/native_f32_actor_turn_lease_v1_test.c \
      -o "$tmp/f32-actor-turn-lease"
    ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 \
      UBSAN_OPTIONS=halt_on_error=1 "$tmp/f32-actor-turn-lease"
    echo "ORES_F32_NATIVE_ADMISSION_MATRIX_PASS|cc=$cc|opt=$opt|turn_pinning=EXECUTED_HOST_ONLY|guest=NOT_EXECUTED|compiler_lowering=NOT_IMPLEMENTED"
  done
done
