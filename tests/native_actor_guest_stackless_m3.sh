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
source_file="$root/tests/fixtures/actor-guest-stackless-fib-m3.atto"
test -f "$source_file"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# No compiler stdout is allowed for rejected source, including a valid actor
# mixed with an unsupported source main/ActorRef entry point.
cat "$source_file" > "$tmp/invalid.atto"
printf '\npub fnc main(): int { return 0; }\n' >> "$tmp/invalid.atto"
if "$compiler" --experimental-actor-guest-stackless "$tmp/invalid.atto" \
    > "$tmp/invalid.ll" 2> "$tmp/invalid.err"; then
  echo "FAIL: unsupported actor main accepted" >&2
  exit 1
fi
test ! -s "$tmp/invalid.ll"
grep -Fq "actor guest frame M2 requires one native actor-only source module" \
    "$tmp/invalid.err"

# Unflagged compilation must retain the existing source actor ABI,
# without importing the new experimental actor stack primitives.
"$compiler" "$source_file" > "$tmp/unflagged.ll"
test -s "$tmp/unflagged.ll"
if grep -Fq 'oreslang_actor_guest_frame_i64_' "$tmp/unflagged.ll"; then
  echo "FAIL: experimental frame ABI leaked into ordinary actor lowering" >&2
  exit 1
fi

"$compiler" --experimental-actor-guest-stackless "$source_file" > "$tmp/actor.ll"
test -s "$tmp/actor.ll"
grep -Fq "define void @oreslang_actor_Counter_init" "$tmp/actor.ll"
grep -Fq "define i64 @oreslang_actor_Counter_on_message" "$tmp/actor.ll"
grep -Fq "call void @oreslang_actor_guest_frame_i64_enter" "$tmp/actor.ll"
grep -Fq "call i64 @oreslang_actor_guest_frame_i64_read" "$tmp/actor.ll"
grep -Fq "call void @oreslang_actor_guest_frame_i64_leave" "$tmp/actor.ll"
grep -Fq "call i64 @oreslang_actor_guest_stackless_i64_execute" "$tmp/actor.ll"
grep -Fq "atto.actor.stackless.m3.Counter.0" "$tmp/actor.ll"
if grep -Eq 'llvm\.gcroot|llvm\.experimental\.gc\.statepoint|gc "' "$tmp/actor.ll"; then
  echo "FAIL: unexpected tracing-GC IR marker" >&2
  exit 1
fi
# A helper call reading mutable actor state outside the parameter-only
# M3 bytecode contract must fail before LLVM IR can be published.
sed 's/total = score(n);/total = score(total);/' "$source_file" > "$tmp/invalid-field.atto"
if "$compiler" --experimental-actor-guest-stackless "$tmp/invalid-field.atto" \
    > "$tmp/invalid-field.ll" 2> "$tmp/invalid-field.err"; then
  echo "FAIL: actor state leaked into M3 scalar helper bytecode" >&2
  exit 1
fi
test ! -s "$tmp/invalid-field.ll"
grep -Fq "actor M3 frame expression may access only its scalar parameter" \
    "$tmp/invalid-field.err"

sha256sum "$compiler" "$source_file" "$tmp/actor.ll"

for mode in 0 2; do
  clang -std=c11 -O"$mode" -pthread -Wall -Wextra -Wpedantic -Werror \
    -I"$root/src/main/cpp" \
    -c "$root/src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c" \
    -o "$tmp/heap-$mode.o"
  clang++ -std=c++17 -O"$mode" -pthread -Wall -Wextra -Wpedantic -Werror \
    -Wno-error=override-module -UNDEBUG \
    -I"$root/src/main/cpp" \
    "$root/tests/native_actor_guest_stackless_m3_test.cpp" \
    "$root/src/main/cpp/oreslang/runtime/MultiActorCarrier.cpp" \
    "$tmp/heap-$mode.o" "$tmp/actor.ll" -o "$tmp/guest-$mode"
  "$tmp/guest-$mode" | tee "$tmp/guest-$mode.log"
  grep -Fqx ATTO_LLVM_COMPILED_STACKLESS_ACTOR_M3_PASS "$tmp/guest-$mode.log"
  sha256sum "$tmp/guest-$mode"

  # A recursive fib(48) must deterministically exceed the finite actor
  # frame depth and abort, not C-stack recurse or silently wrap the counter.
  ulimit -c 0
  set +e
  "$tmp/guest-$mode" --overflow > "$tmp/overflow-$mode.out" 2>&1
  overflow_status=$?
  set -e
  if [[ "$overflow_status" -ne 134 ]]; then
    echo "FAIL: M3 overflow process exited $overflow_status (expected SIGABRT/134)" >&2
    cat "$tmp/overflow-$mode.out" >&2
    exit 1
  fi
done
echo "ATTO_LLVM_COMPILER_STACKLESS_M3_O0_O2_PASS"
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
    "$root/tests/native_actor_guest_stackless_m3_test.cpp" \
    "$root/src/main/cpp/oreslang/runtime/MultiActorCarrier.cpp" \
    "$tmp/heap-sanitized.o" "$tmp/actor.ll" -o "$tmp/guest-sanitized"
  "$tmp/guest-sanitized" | tee "$tmp/guest-sanitized.log"
  grep -Fqx ATTO_LLVM_COMPILED_STACKLESS_ACTOR_M3_PASS "$tmp/guest-sanitized.log"
  sha256sum "$tmp/guest-sanitized"
  echo "ATTO_LLVM_COMPILER_STACKLESS_M3_ASAN_UBSAN_PASS"
fi
