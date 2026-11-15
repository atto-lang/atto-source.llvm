#!/usr/bin/env bash
# Verify native pointer HOST tests at an exact Git commit. Never promotes
# unexecuted/unsupported guest AOT claims to a PASS result.
set -Eeuo pipefail

usage() {
  echo "Usage: $0 EXACT_COMMIT_SHA [RECEIPT_DIRECTORY]" >&2
  echo "Requires Linux, Git, CMake, CTest, GCC/G++, Clang/Clang++, SHA-256." >&2
}
if [[ $# -lt 1 || $# -gt 2 || ! $1 =~ ^[0-9a-f]{40}$ ]]; then
  usage
  exit 2
fi
expected_sha=$1
if [[ $(uname -s) != Linux ]]; then
  echo 'UNSUPPORTED: Linux-only GCC/Clang native host verification' >&2
  exit 2
fi
for tool in git cmake ctest gcc g++ clang clang++ sha256sum; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "NOT_EXECUTED: required tool unavailable: $tool" >&2
    exit 2
  fi
done
root=$(git rev-parse --show-toplevel)
cd "$root"
actual_sha=$(git rev-parse --verify HEAD)
# A receipt from an unrelated or synthetic repo must never claim LLVM source
# provenance. Do not print or persist credential-bearing remote URLs.
origin=$(git remote get-url origin 2>/dev/null || true)
if [[ ! $origin =~ ^(https://([^/?#@]+@)?github[.]com/|git@github[.]com:|ssh://git@github[.]com/)ores-truffle-oreslang/oreslang-source[.]llvm([.]git)?$ ]]; then
  echo 'FAIL: checkout origin is not the approved LLVM source repository' >&2
  exit 2
fi
if [[ $expected_sha != "$actual_sha" ]]; then
  echo "FAIL: expected HEAD $expected_sha, actual $actual_sha" >&2
  exit 2
fi
if [[ -n $(git status --porcelain --untracked-files=normal) ]]; then
  echo 'FAIL: checkout is dirty; exact-commit execution is not certified' >&2
  exit 2
fi
if [[ ! -f src/main/cpp/oreslang/runtime/NativeActorSupervisor.hpp || \
      ! -f tests/native_actor_supervisor_pointer_integration_test.cpp ]]; then
  echo 'FAIL: native pointer test files are absent from exact HEAD' >&2
  exit 2
fi
receipt=${2:-$(mktemp -d "${TMPDIR:-/tmp}/atto-native-pointer.XXXXXXXX")}
mkdir -p "$receipt"
receipt=$(cd "$receipt" && pwd -P)
if [[ $receipt == "$root" || $receipt == "$root/"* ]]; then
  echo 'FAIL: receipt must be outside checkout to preserve clean-tree evidence' >&2
  exit 2
fi
mkdir -p "$receipt/logs"
status_file="$receipt/stages.tsv"
printf 'stage\tstatus\tlog\n' > "$status_file"
finish() {
  local rc=$?
  {
    printf 'expected_commit=%s\n' "$expected_sha"
    printf 'observed_commit=%s\n' "$actual_sha"
    printf 'git_tree=%s\n' "$(git rev-parse 'HEAD^{tree}')"
    # Never record credential-bearing remote URLs in a published receipt.
    printf 'repository=ores-truffle-oreslang/oreslang-source.llvm\n'
    printf 'host_os=%s\n' "$(uname -srm)"
    printf 'host_status=%s\n' "$([[ $rc -eq 0 ]] && echo EXECUTED_PASS || echo EXECUTED_FAIL_OR_INCOMPLETE)"
    printf 'atto_guest_aot=NOT_EXECUTED\n'
    printf 'cross_backend_parity=NOT_CERTIFIED\n'
  } > "$receipt/manifest.txt"
  (cd "$receipt" && find logs -type f -print0 | sort -z | xargs -0 -r sha256sum) \
    > "$receipt/log_sha256.txt"
  (cd "$receipt" && find . -type f -path './build-*' -name '*_host_test' -print0 | sort -z | xargs -0 -r sha256sum) \
    > "$receipt/binary_sha256.txt"
  echo "Native pointer evidence receipt: $receipt" >&2
}
trap finish EXIT
{
  git --version
  cmake --version | head -1
  gcc --version | head -1
  g++ --version | head -1
  clang --version | head -1
  clang++ --version | head -1
} > "$receipt/toolchains.txt"

run_stage() {
  local name=$1
  shift
  if "$@" > "$receipt/logs/$name.log" 2>&1; then
    printf '%s\tEXECUTED_PASS\t%s\n' "$name" "logs/$name.log" >> "$status_file"
  else
    printf '%s\tEXECUTED_FAIL\t%s\n' "$name" "logs/$name.log" >> "$status_file"
    echo "FAIL: $name; see $receipt/logs/$name.log" >&2
    exit 1
  fi
}

# Three separate configured builds prevent sanitizer flag contamination.
# The user must keep the source tree on the exact immutable PR commit.
for profile in gcc-release clang-asan-ubsan gcc-tsan; do
  case "$profile" in
    gcc-release)
      cc=gcc; cxx=g++; build_type=Release; flags='-O2'; ldflags='';;
    clang-asan-ubsan)
      cc=clang; cxx=clang++; build_type=Debug
      flags='-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer'
      ldflags='-fsanitize=address,undefined';;
    gcc-tsan)
      cc=gcc; cxx=g++; build_type=Debug
      flags='-O1 -g -fsanitize=thread -fno-omit-frame-pointer'
      ldflags='-fsanitize=thread';;
  esac
  build="$receipt/build-$profile"
  run_stage "configure-$profile" cmake -S "$root" -B "$build" \
    -DATTO_NATIVE_POINTER_HOST_ONLY=ON \
    -DCMAKE_BUILD_TYPE="$build_type" \
    -DCMAKE_C_COMPILER="$cc" -DCMAKE_CXX_COMPILER="$cxx" \
    -DCMAKE_C_FLAGS="$flags" -DCMAKE_CXX_FLAGS="$flags" \
    -DCMAKE_EXE_LINKER_FLAGS="$ldflags"
  run_stage "build-$profile" cmake --build "$build" --parallel 2
  # CTest's suite names alone are not evidence if the host binaries were
  # replaced by stub commands. Require the four expected native targets.
  for exe in atto_trusted_pointer_window_host_test \
             atto_native_region_registry_host_test \
             atto_native_region_runtime_authority_host_test \
             atto_native_actor_supervisor_pointer_host_test; do
    if [[ ! -x "$build/$exe" ]]; then
      printf '%s\tEXECUTED_FAIL\t%s\n' "required-binary-$profile" "logs/build-$profile.log" >> "$status_file"
      echo "FAIL: expected native security binary $exe not built for $profile" >&2
      exit 1
    fi
  done
  # CTest's default success for ZERO tests must never certify an empty suite.
  run_stage "enumerate-$profile" ctest --test-dir "$build" -N
  listed="$receipt/logs/enumerate-$profile.log"
  count=$(sed -n 's/^Total Tests: \([0-9][0-9]*\).*$/\1/p' "$listed" | tail -1)
  if [[ ! $count =~ ^[0-9]+$ || $count -lt 5 ]] ||
     ! grep -Fq 'atto_native_actor_supervisor_pointer_host_only' "$listed" ||
     ! grep -Fq 'atto_native_pointer_negative_compile' "$listed"; then
    printf '%s\tEXECUTED_FAIL\t%s\n' "test-manifest-$profile" "logs/enumerate-$profile.log" >> "$status_file"
    echo "FAIL: native pointer CTest manifest missing required cases for $profile" >&2
    exit 1
  fi
  run_stage "test-$profile" env CXX="$cxx" ctest --test-dir "$build" \
    --output-on-failure --timeout 120
done
