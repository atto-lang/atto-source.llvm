#!/usr/bin/env bash
# Actual native ActorArena/ActorRegionV1 code, NOT an Atto guest compiler run.
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="$(mktemp -d)"
trap 'rm -rf "$build_dir"' EXIT
for cc in clang gcc; do
  command -v "$cc" >/dev/null || { echo "Required compiler unavailable: $cc" >&2; exit 1; }
  for opt in 0 2; do
    bin="$build_dir/actor-finish-${cc}-O${opt}"
    "$cc" -std=c11 -O"$opt" -g -Wall -Wextra -Wpedantic -Werror \
      -fsanitize=address,undefined -fno-omit-frame-pointer \
      -I "$repo_root/src/main/cpp" \
      "$repo_root/tests/actor_region_compiler_finish_v1_test.c" \
      "$repo_root/src/main/cpp/oreslang/runtime/ActorRegionV1.c" \
      "$repo_root/src/main/cpp/oreslang/runtime/ActorArena.c" \
      -o "$bin"
    output="$(ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 \
      UBSAN_OPTIONS=halt_on_error=1 "$bin" 2>"$build_dir/stderr")" || {
      cat "$build_dir/stderr" >&2; exit 1;
    }
    [[ "$output" == "ACTOR_REGION_COMPILER_ATOMIC_FINISH_HOST_PASS" ]] || {
      echo "Unexpected native evidence: $output" >&2; exit 1;
    }
    echo "PASS native actor arena checked finish: $cc -O$opt"
  done
done
