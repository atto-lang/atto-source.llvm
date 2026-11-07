#!/usr/bin/env bash
# Exact-source LLVM ActorGroup negative/host registry verification gate.
set -euo pipefail
cd "$(dirname "$0")/.."

for tool in cmake ctest c++; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "Missing prerequisite: $tool" >&2
    exit 2
  fi
done
# A merge gate must prove the exact checked-out tree, not a partial or
# locally modified mirror. Callers may pin a PR HEAD with this environment
# variable; unexpected commits fail before touching the compiler.
if ! command -v git >/dev/null 2>&1 || ! git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  echo "ActorGroup promotion requires a Git checkout" >&2
  exit 2
fi
actual_sha="$(git rev-parse HEAD)"
echo "ActorGroup exact-source checkout: $actual_sha"
if [[ -n "${ORESLANG_ACTORGROUP_EXPECTED_SHA:-}" &&
      "$actual_sha" != "$ORESLANG_ACTORGROUP_EXPECTED_SHA" ]]; then
  echo "Wrong revision: expected $ORESLANG_ACTORGROUP_EXPECTED_SHA, got $actual_sha" >&2
  exit 2
fi
if [[ -n "$(git status --porcelain)" ]]; then
  echo "Dirty working tree: refusing to certify an uncommitted source state" >&2
  git status --short >&2
  exit 2
fi

build_dir="${ORESLANG_ACTORGROUP_BUILD_DIR:-build/verify-actorgroup}"
cmake -S . -B "$build_dir" -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build "$build_dir" --parallel "${ORESLANG_BUILD_JOBS:-4}"
# Every mandatory CTest must actually be registered. By default ctest
# succeeds with zero selected tests, which would be a false green gate.
required_tests=(
  reject_guest_actor_group_create
  reject_guest_actor_group_join
  reject_guest_actor_group_shadow
  native_actor_multi_group_membership
  native_actor_group_managed_worker_readiness
  source_actor_compiled_native_lifecycle
  source_actor_compiled_native_asan_ubsan
)

# Require each *exact* name before testing. ctest with zero matches returns
# success, so a filtered ctest command alone is not a valid promotion gate.
for test_name in "${required_tests[@]}"; do
  if ! ctest --test-dir "$build_dir" -N -R "^${test_name}$" |
       grep -Eq 'Total Tests: 1$'; then
    echo "Missing or duplicate mandatory ActorGroup CTest: $test_name" >&2
    exit 2
  fi
done
test_regex="^($(IFS='|'; echo "${required_tests[*]}"))$"
ctest --test-dir "$build_dir" --output-on-failure -R "$test_regex"

# Standalone native registry test under ASan/UBSan, independent of LLVM guest
# lowering. Run once, after confirming the required CTests actually execute.
sanitized_dir="$(mktemp -d "${TMPDIR:-/tmp}/oreslang-actorgroup-asan.XXXXXX")"
trap 'rm -rf "$sanitized_dir"' EXIT
sanitized="$sanitized_dir/native_actor_membership_test"
c++ -std=c++17 -pthread -O1 -g -Wall -Wextra -Wpedantic -Werror \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I src/main/cpp tests/native_actor_membership_test.cpp \
  -o "$sanitized"
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
  UBSAN_OPTIONS=halt_on_error=1 "$sanitized"

if [[ "${ORESLANG_ACTORGROUP_FULL:-0}" == 1 ]]; then
  ctest --test-dir "$build_dir" --output-on-failure
fi
