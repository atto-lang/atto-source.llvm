#!/usr/bin/env bash
# Compile the ACTUAL native C VM, C actor scheduler, C++ epoll manager/ABI and
# lifecycle tests together. This source-local gate does not move private code
# to another organization or claim .ores source-level await parity.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"
head_sha="$(git rev-parse --verify HEAD)"
tree_sha="$(git rev-parse --verify "HEAD^{tree}")"
# An immutable commit/tree does not prove a matching worktree: local Git config
# and index bits can conceal chmod, ignored inputs and source mutations.
# This helper uses NUL-framed Git output, forces fileMode/fsmonitor overrides,
# and admits only ignored build/cache output directories at the repository root.
command -v python3 >/dev/null || { printf 'Python 3 required for NIO source-integrity gate\n' >&2; exit 2; }
verify_exact_worktree() {
  python3 -I scripts/verify-native-nio-source-local.py "$head_sha" "$tree_sha"
}
verify_exact_worktree
if [[ -n "${ORES_NIO_EXPECT_HEAD:-}" && "$head_sha" != "$ORES_NIO_EXPECT_HEAD" ]]; then
  printf 'Exact-head mismatch: expected %s, checked out %s\n' "$ORES_NIO_EXPECT_HEAD" "$head_sha" >&2
  exit 2
fi
if [[ -n "${ORES_NIO_EXPECT_TREE:-}" && "$tree_sha" != "$ORES_NIO_EXPECT_TREE" ]]; then
  printf 'Exact-tree mismatch: expected %s, checked out %s\n' "$ORES_NIO_EXPECT_TREE" "$tree_sha" >&2
  exit 2
fi
printf 'Testing exact source head=%s tree=%s\n' "$head_sha" "$tree_sha"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
source_root="src/main/cpp"
runtime="$source_root/oreslang/runtime"
test_source="tests/native_vm_nio_lifecycle_test.c"

for toolchain in gcc clang; do
  cc="$toolchain"
  cxx="$([[ "$toolchain" == gcc ]] && printf 'g++' || printf 'clang++')"
  command -v "$cc" >/dev/null
  command -v "$cxx" >/dev/null
  dest="$tmp/$toolchain"
  mkdir -p "$dest"
  shared=(-O1 -g -pthread -fsanitize=address,undefined -fno-omit-frame-pointer
          -Wall -Wextra -Wpedantic -Werror -I"$source_root")
  for name in NativeOresVM NativeActorScheduler; do
    "$cc" -std=c11 -D_GNU_SOURCE "${shared[@]}" -c "$runtime/$name.c" -o "$dest/$name.o"
  done
  "$cc" -std=c11 "${shared[@]}" -c "$test_source" -o "$dest/lifecycle.o"
  for name in NativeOresVMNio NativeEpollReactorABI; do
    "$cxx" -std=c++17 "${shared[@]}" -c "$runtime/$name.cpp" -o "$dest/$name.o"
  done
  "$cxx" "${shared[@]}" "$dest"/*.o -o "$dest/lifecycle"
  for attempt in {1..30}; do
    printf "NIO_LIFECYCLE_ATTEMPT=%s toolchain=%s\n" "$attempt" "$toolchain"
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
    UBSAN_OPTIONS=halt_on_error=1 \
      timeout 60s "$dest/lifecycle"
  done
  printf 'PASS: %s C VM + C++ epoll lifecycle / close races (ASan+UBSan)\n' "$toolchain"
  # Include the byte-identical standalone epoll and bootstrap-failure
  # fixtures in the source-local native sanitizer gate.
  for fixture in native_epoll_reactor_test native_epoll_reactor_bootstrap_failure_test; do
    "$cxx" -std=c++17 "${shared[@]}" "tests/$fixture.cpp" -o "$dest/$fixture"
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
    UBSAN_OPTIONS=halt_on_error=1 \
      timeout 60s "$dest/$fixture"
    printf 'PASS: %s %s (ASan+UBSan)\n' "$toolchain" "$fixture"
  done
  # Separately exercise host-only descriptor authority races under
  # both toolchains. This does NOT prove guest FD authorization integration.
  "$cxx" -std=c++17 "${shared[@]}" \
    tests/native_fd_authority_test.cpp -o "$dest/fd-authority"
  for attempt in {1..3}; do
    printf 'FD_AUTHORITY_HOST_MODEL_ATTEMPT=%s toolchain=%s\n' "$attempt" "$toolchain"
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
    UBSAN_OPTIONS=halt_on_error=1 \
      timeout 60s "$dest/fd-authority"
  done
  # Execute the real epoll/owned-eventfd broker, separately from the guest ABI.
  "$cxx" -std=c++17 "${shared[@]}" \
    tests/native_fd_authority_broker_test.cpp -o "$dest/fd-broker"
  for attempt in {1..3}; do
    printf "NIO_REAL_EPOLL_FD_BROKER_ATTEMPT=%s toolchain=%s\n" "$attempt" "$toolchain"
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
    UBSAN_OPTIONS=halt_on_error=1 \
      timeout 60s "$dest/fd-broker"
  done
  verify_exact_worktree
done

# Independent source parser admission for @ActorMaxThreads(LOW) and rejection
# of MEDIUM/HIGH. This is host frontend validation, not parallel actors.
for toolchain in gcc clang; do
  cxx="$([[ "$toolchain" == gcc ]] && printf 'g++' || printf 'clang++')"
  "$cxx" -std=c++17 -O1 -g -Wall -Wextra -Wpedantic -Werror -pthread \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -I"$source_root" \
    tests/actor_max_threads_source_admission_test.cpp \
    "$source_root/oreslang/frontend/Parser.cpp" \
    "$source_root/oreslang/types/OwnershipChecker.cpp" \
    -o "$tmp/actor-max-threads-$toolchain"
  ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
  UBSAN_OPTIONS=halt_on_error=1 \
    timeout 30s "$tmp/actor-max-threads-$toolchain"
  printf 'PASS: %s typed actor-thread frontend admission (ASan+UBSan)\n' "$toolchain"
  verify_exact_worktree
done

# Final source identity must match the exact tree that passed admission.
verify_exact_worktree
