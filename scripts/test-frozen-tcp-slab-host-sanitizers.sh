#!/usr/bin/env bash
set -euo pipefail
# Executes actual native C++ host slab tests, NOT compiled guest actor code.
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CXX="${CXX:-g++}"
MODE="${SANITIZER_MODE:-address-undefined}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
case "$MODE" in
  address-undefined)
    sanitizers=(-fsanitize=address,undefined)
    ;;
  thread)
    sanitizers=(-fsanitize=thread)
    ;;
  *)
    echo "unsupported native sanitizer mode: $MODE" >&2
    exit 2
    ;;
esac
"$CXX" -std=c++20 -O1 -g -pthread -Wall -Wextra -Werror -pedantic \
  "${sanitizers[@]}" -fno-omit-frame-pointer \
  -I"$ROOT/src/main/cpp" "$ROOT/tests/frozen_tcp_slab_test.cpp" \
  -o "$WORK/frozen_tcp_slab_test"
"$CXX" -std=c++20 -O1 -g -pthread -Wall -Wextra -Werror -pedantic \
  "${sanitizers[@]}" -fno-omit-frame-pointer \
  -I"$ROOT/src/main/cpp" "$ROOT/tests/actor_shared_tcp_pool_test.cpp" \
  -o "$WORK/actor_shared_tcp_pool_test"
if [[ "$MODE" == "thread" ]]; then
  TSAN_OPTIONS="halt_on_error=1" "$WORK/frozen_tcp_slab_test"
  TSAN_OPTIONS="halt_on_error=1" "$WORK/actor_shared_tcp_pool_test"
else
  ASAN_OPTIONS="detect_leaks=1" "$WORK/frozen_tcp_slab_test"
  ASAN_OPTIONS="detect_leaks=1" "$WORK/actor_shared_tcp_pool_test"
fi
printf 'NATIVE_FROZEN_TCP_SLAB_%s_PASS\n' "$MODE"
