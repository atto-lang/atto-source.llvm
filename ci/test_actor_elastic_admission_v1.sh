#!/usr/bin/env bash
# HOST-ONLY native scheduler substrate tests; not compiled .atto guest proof.
set -euo pipefail
cd "$(dirname "$0")/.."
temp="$(mktemp -d)"
trap 'rm -rf "$temp"' EXIT
for compiler in gcc clang; do
  if ! command -v "$compiler" >/dev/null 2>&1; then
    echo "required compiler missing: $compiler" >&2
    exit 1
  fi
  for profile in baseline sanitizer; do
    flags=(-std=c11 -Wall -Wextra -Werror -UNDEBUG -g -O1 -fno-omit-frame-pointer -pthread -Iruntime)
    if [[ "$profile" == sanitizer ]]; then
      flags+=(-fsanitize=address,undefined)
    fi
    "$compiler" "${flags[@]}" runtime/actor_elastic_admission_v1.c \
      tests/actor_elastic_admission_v1_test.c -o "$temp/$compiler-$profile"
    ASAN_OPTIONS=detect_leaks=1 "$temp/$compiler-$profile"
  done
done
echo "SOURCE_FREE_NATIVE_HOST_ONLY: no AttoLang guest compiler or .atto runtime executed"
