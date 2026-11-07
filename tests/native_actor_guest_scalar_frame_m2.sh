#!/usr/bin/env bash
# Actual .atto actor source -> oreslang-llvmc -> LLVM IR -> Clang C++ native
# -> real MultiActorCarrier worker / ActorOwnedI64Heap, NOT a host model.
set -euo pipefail
if [[ $# -lt 2 || $# -gt 3 || ( $# -eq 3 && "${3}" != "--sanitized" ) ]]; then
  echo "usage: $0 <absolute-oreslang-llvmc> <source-root> [--sanitized]" >&2
  exit 2
fi
compiler="$1"
root="$2"
command -v clang >/dev/null
command -v clang++ >/dev/null
command -v sha256sum >/dev/null
test -x "$compiler"
source_file="$root/tests/fixtures/actor-guest-scalar-frame-m2.atto"
test -f "$source_file"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# No compiler stdout is allowed for rejected source, including a valid actor
# mixed with an unsupported source main/ActorRef entry point.
cat "$source_file" > "$tmp/invalid.atto"
printf '\npub fnc main(): int { return 0; }\n' >> "$tmp/invalid.atto"
if "$compiler" --experimental-actor-guest-frames "$tmp/invalid.atto" \
    > "$tmp/invalid.ll" 2> "$tmp/invalid.err"; then
  echo "FAIL: unsupported actor main accepted" >&2
  exit 1
fi
test ! -s "$tmp/invalid.ll"
grep -Fq "actor guest frame M2 requires one native actor-only source module" \
    "$tmp/invalid.err"

"$compiler" --experimental-actor-guest-frames "$source_file" > "$tmp/actor.ll"
test -s "$tmp/actor.ll"
grep -Fq "define void @oreslang_actor_Counter_init" "$tmp/actor.ll"
grep -Fq "define i64 @oreslang_actor_Counter_on_message" "$tmp/actor.ll"
grep -Fq "call void @oreslang_actor_guest_frame_i64_enter" "$tmp/actor.ll"
grep -Fq "call i64 @oreslang_actor_guest_frame_i64_read" "$tmp/actor.ll"
grep -Fq "call void @oreslang_actor_guest_frame_i64_leave" "$tmp/actor.ll"
if grep -Eq 'llvm\.gcroot|llvm\.experimental\.gc\.statepoint|gc "' "$tmp/actor.ll"; then
  echo "FAIL: unexpected tracing-GC IR marker" >&2
  exit 1
fi
sha256sum "$compiler" "$source_file" "$tmp/actor.ll"

for mode in 0 2; do
  clang -std=c11 -O"$mode" -pthread -Wall -Wextra -Wpedantic -Werror \
    -I"$root/src/main/cpp" \
    -c "$root/src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c" \
    -o "$tmp/heap-$mode.o"
  clang++ -std=c++17 -O"$mode" -pthread -Wall -Wextra -Wpedantic -Werror \
    -Wno-error=override-module -UNDEBUG \
    -I"$root/src/main/cpp" \
    "$root/tests/native_actor_guest_scalar_frame_m2_test.cpp" \
    "$root/src/main/cpp/oreslang/runtime/MultiActorCarrier.cpp" \
    "$tmp/heap-$mode.o" "$tmp/actor.ll" -o "$tmp/guest-$mode"
  "$tmp/guest-$mode" | tee "$tmp/guest-$mode.log"
  grep -Fx ATTO_LLVM_COMPILED_ACTOR_GUEST_FRAME_M2_PASS "$tmp/guest-$mode.log"
  sha256sum "$tmp/guest-$mode"
done
echo "ATTO_LLVM_COMPILER_FRAME_M2_O0_O2_PASS"
if [[ "${3:-}" == "--sanitized" ]]; then
  clang -std=c11 -O1 -g -pthread -fsanitize=address,undefined \
    -fno-omit-frame-pointer -Wall -Wextra -Wpedantic -Werror \
    -I"$root/src/main/cpp" \
    -c "$root/src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c" \
    -o "$tmp/heap-sanitized.o"
  clang++ -std=c++17 -O1 -g -pthread -fsanitize=address,undefined \
    -fno-omit-frame-pointer -Wall -Wextra -Wpedantic -Werror \
    -Wno-error=override-module -UNDEBUG \
    -I"$root/src/main/cpp" \
    "$root/tests/native_actor_guest_scalar_frame_m2_test.cpp" \
    "$root/src/main/cpp/oreslang/runtime/MultiActorCarrier.cpp" \
    "$tmp/heap-sanitized.o" "$tmp/actor.ll" -o "$tmp/guest-sanitized"
  "$tmp/guest-sanitized" | tee "$tmp/guest-sanitized.log"
  grep -Fx ATTO_LLVM_COMPILED_ACTOR_GUEST_FRAME_M2_PASS "$tmp/guest-sanitized.log"
  sha256sum "$tmp/guest-sanitized"
  echo "ATTO_LLVM_COMPILER_FRAME_M2_ASAN_UBSAN_PASS"
fi
