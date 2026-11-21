#!/usr/bin/env bash
# Exact-checkout, secrets-free host evidence. DOES NOT certify .ores execution.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"
if [[ -n "$(git status --porcelain --untracked-files=all)" ]]; then
  echo "FAIL: working tree is not clean; exact-head verification refused" >&2
  exit 2
fi
head="$(git rev-parse --verify HEAD)"
# Optional externally pinned source HEAD is mandatory for a future
# authorized private carrier. Never equate a clean but wrong checkout
# with the intended PR head.
if [[ -n "${EXPECTED_SOURCE_HEAD:-}" && "$head" != "$EXPECTED_SOURCE_HEAD" ]]; then
  echo "FAIL: checkout HEAD $head differs from expected $EXPECTED_SOURCE_HEAD" >&2
  exit 2
fi
tree="$(git rev-parse 'HEAD^{tree}')"
# A token-free cross-org mirror normally creates a DIFFERENT commit SHA.
# Validate immutable source-tree identity, not a guessed equivalent commit.
if [[ -n "${EXPECTED_SOURCE_TREE:-}" && "$tree" != "$EXPECTED_SOURCE_TREE" ]]; then
  echo "FAIL: checkout tree $tree differs from expected $EXPECTED_SOURCE_TREE" >&2
  exit 2
fi
echo "SOURCE_HEAD=$head SOURCE_TREE=$tree"
paths=(
  src/main/cpp/oreslang/runtime/TrustedActorBufferPool.hpp
  src/main/cpp/oreslang/runtime/TrustedActorBufferMailbox.hpp
  src/main/cpp/oreslang/runtime/TrustedActorBufferSchedulerGate.hpp
  src/main/cpp/oreslang/runtime/TrustedActorBufferDelivery.hpp
  tests/trusted_actor_buffer_pool_test.cpp
  tests/trusted_actor_buffer_mailbox_test.cpp
  tests/trusted_actor_buffer_scheduler_gate_test.cpp
  tests/trusted_actor_buffer_delivery_test.cpp
  src/main/cpp/oreslang/runtime/NativeActorScheduler.h
  src/main/cpp/oreslang/runtime/NativeActorScheduler.c
  tests/native_actor_owned_buffer_scheduler_test.c
)
for path in "${paths[@]}"; do
  expected="$(git rev-parse "HEAD:$path")"
  actual="$(git hash-object "$path")"
  if [[ "$actual" != "$expected" ]]; then
    echo "FAIL: source blob mismatch $path" >&2
    exit 2
  fi
  printf 'BLOB %s %s\n' "$actual" "$path"
done
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
names=(
  trusted_actor_buffer_pool
  trusted_actor_buffer_mailbox
  trusted_actor_buffer_scheduler_gate
  trusted_actor_buffer_delivery
)
for compiler in g++ clang++; do
  command -v "$compiler" >/dev/null || { echo "missing compiler: $compiler" >&2; exit 2; }
  "$compiler" --version | head -n 1
  for name in "${names[@]}"; do
    "$compiler" -std=c++17 -pthread -O1 -g -UNDEBUG \
      -Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined \
      -fno-omit-frame-pointer -Isrc/main/cpp "tests/${name}_test.cpp" \
      -o "$tmp/${name}.asan"
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
      UBSAN_OPTIONS=halt_on_error=1 "$tmp/${name}.asan"
    echo "PASS ${compiler} ASAN_UBSAN ${name} HEAD=${head}"
  done
done
command -v g++ >/dev/null
for name in "${names[@]}"; do
  g++ -std=c++17 -pthread -O1 -g -UNDEBUG \
    -Wall -Wextra -Wpedantic -Werror -fsanitize=thread \
    -fno-omit-frame-pointer -Isrc/main/cpp "tests/${name}_test.cpp" \
    -o "$tmp/${name}.tsan"
  TSAN_OPTIONS=halt_on_error=1 "$tmp/${name}.tsan"
  echo "PASS GCC_TSAN ${name} HEAD=${head}"
done
# Mixed scalar/owned-buffer DATA is exercised on the real C scheduler;
# mock host ownership hooks are NOT a compiler-generated .ores guest.
for compiler in gcc clang; do
  command -v "$compiler" >/dev/null || { echo "missing compiler: $compiler" >&2; exit 2; }
  "$compiler" -std=c11 -pthread -O1 -g -UNDEBUG \
    -Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined \
    -fno-omit-frame-pointer -Isrc/main/cpp \
    tests/native_actor_owned_buffer_scheduler_test.c \
    src/main/cpp/oreslang/runtime/NativeActorScheduler.c \
    -o "$tmp/native-actor-tagged.$compiler"
  ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
    UBSAN_OPTIONS=halt_on_error=1 "$tmp/native-actor-tagged.$compiler"
  echo "PASS ${compiler} NATIVE_TAGGED_ASAN_UBSAN HEAD=$head TREE=$tree"
done
gcc -std=c11 -pthread -O1 -g -UNDEBUG \
  -Wall -Wextra -Wpedantic -Werror -fsanitize=thread \
  -fno-omit-frame-pointer -Isrc/main/cpp \
  tests/native_actor_owned_buffer_scheduler_test.c \
  src/main/cpp/oreslang/runtime/NativeActorScheduler.c \
  -o "$tmp/native-actor-tagged.tsan"
TSAN_OPTIONS=halt_on_error=1 "$tmp/native-actor-tagged.tsan"
echo "PASS gcc NATIVE_TAGGED_TSAN HEAD=$head TREE=$tree"
echo "PASS HOST_ONLY_SOURCE_HEAD=$head SOURCE_TREE=$tree tests=15 guest_ores=NOT_TESTED"
