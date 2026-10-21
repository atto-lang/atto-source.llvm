#!/usr/bin/env bash
# Execute actual frontend passes and native heaps without LLVM development libs.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT
cc="${CC:-gcc}"
cxx="${CXX:-g++}"
mode="${1:-optimized}"
flags=(-O2 -DNDEBUG)
wrap=(-DORES_ARENA_TEST_ALLOCATIONS=1 -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free)
case "$mode" in
  optimized) ;;
  sanitized) flags=(-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer); wrap=() ;;
  *) echo 'expected optimized or sanitized' >&2; exit 64 ;;
esac
common=(-Wall -Wextra -Wpedantic -Werror -I"$root/src/main/cpp")
"$cxx" -std=c++17 "${common[@]}" "${flags[@]}" -c "$root/src/main/cpp/oreslang/frontend/Parser.cpp" -o "$work/parser.o"
"$cxx" -std=c++17 "${common[@]}" "${flags[@]}" -c "$root/src/main/cpp/oreslang/types/OwnershipChecker.cpp" -o "$work/ownership.o"
for pass in ownership reachability function_parity; do
  "$cxx" -std=c++17 "${common[@]}" "${flags[@]}" "$root/tests/native_actor_${pass}_test.cpp" "$work/parser.o" "$work/ownership.o" -o "$work/$pass"
  "$work/$pass"
done
"$cc" -std=c11 "${common[@]}" "${flags[@]}" -c "$root/src/main/cpp/oreslang/runtime/ActorArena.c" -o "$work/arena.o"
"$cxx" -std=c++17 "${common[@]}" "${flags[@]}" "$root/tests/native_actor_arena_test.cpp" "$work/arena.o" -pthread "${wrap[@]}" -o "$work/arena"
"$work/arena"
"$cc" -std=c11 "${common[@]}" "${flags[@]}" -c "$root/src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c" -o "$work/heap.o"
"$cxx" -std=c++17 "${common[@]}" "${flags[@]}" "$root/tests/native_actor_scalar_heap_safety_test.cpp" "$work/heap.o" -pthread "${wrap[@]}" -o "$work/heap"
"$work/heap"
"$cxx" -std=c++17 "${common[@]}" "${flags[@]}" "$root/tests/actor_owned_heap_lifecycle_test.cpp" "$work/heap.o" -pthread -o "$work/lifecycle"
"$work/lifecycle"
"$cc" -std=c11 "${common[@]}" "${flags[@]}" "$root/tests/source_actor_heap_host_bridge_test.c" \
  "$root/src/main/cpp/oreslang/runtime/SourceActorBootstrap.c" \
  "$root/src/main/cpp/oreslang/runtime/NativeActorScheduler.c" \
  "$root/src/main/cpp/oreslang/runtime/NativeOresVM.c" "$work/heap.o" -pthread -o "$work/bridge"
"$work/bridge"
"$cxx" -std=c++17 "${common[@]}" "${flags[@]}" -I"$root/src/main/cpp/oreslang/runtime" \
  "$root/tests/native_memory_model_test.cpp" "$work/arena.o" "$work/heap.o" -pthread -o "$work/memory-model"
for source in NativeActorScheduler NativeOresVM SourceActorBootstrap; do
  "$cc" -std=c11 "${common[@]}" "${flags[@]}" -c "$root/src/main/cpp/oreslang/runtime/$source.c" -o "$work/$source.o"
done
"$cxx" -std=c++17 "${common[@]}" "${flags[@]}" "$root/tests/native_actor_model_test.cpp" \
  "$work/heap.o" "$work/NativeActorScheduler.o" "$work/NativeOresVM.o" "$work/SourceActorBootstrap.o" \
  -pthread -o "$work/actor-model"
python3 "$root/scripts/test_native_memory_parity.py" --runner "$work/memory-model" --actor-runner "$work/actor-model"
