#!/usr/bin/env bash
set -euo pipefail
repo="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$repo"
expected="${EXPECTED_CARRIER_HEAD_SHA:-}"
if [[ ! "$expected" =~ ^[0-9a-f]{40}$ ]] || [[ "$(git rev-parse HEAD)" != "$expected" ]]; then
  echo 'M5_LLVM_ASYNC_NOT_EXECUTED: exact source head mismatch' >&2; exit 2
fi
git diff --quiet HEAD -- || { echo 'M5_LLVM_ASYNC_NOT_EXECUTED: checkout dirty' >&2; exit 2; }
test -f "$repo/tests/tenant-vm/guests.c" || {
  echo 'M5_LLVM_ASYNC_NOT_EXECUTED: actual original ABI guest source missing' >&2; exit 2;
}
test -f "$repo/src/main/cpp/oreslang/runtime/TenantVm.cpp" || exit 2
clang -std=c11 -O1 -fno-stack-protector -fno-sanitize=all \
  -Wno-gnu-designator -Wno-c99-designator \
  -I "$repo/src/main/cpp" -I "$repo/tests/tenant-vm" \
  -c tests/tenant-vm/guests.c -o build/atto-tenant-full-source/m5-guests.o
clang++ -std=c++17 -O1 -g -pthread -Wall -Wextra -Werror \
  -I "$repo/src/main/cpp" -I "$repo/tests/tenant-vm" \
  ci/m5/llvm_async_mailbox_stress.cpp \
  src/main/cpp/oreslang/runtime/TenantVm.cpp \
  build/atto-tenant-full-source/m5-guests.o -ldl -o build/atto-tenant-full-source/m5-llvm-mailbox
timeout 180 build/atto-tenant-full-source/m5-llvm-mailbox \
  > build/atto-tenant-full-source/m5-llvm-output \
  2> build/atto-tenant-full-source/m5-llvm-stderr || {
    echo 'M5_LLVM_ASYNC_HOST_FAILED' >&2
    cat build/atto-tenant-full-source/m5-llvm-output >&2
    cat build/atto-tenant-full-source/m5-llvm-stderr >&2
    exit 1
  }
cat build/atto-tenant-full-source/m5-llvm-output
test ! -s build/atto-tenant-full-source/m5-llvm-stderr || {
  echo 'M5_LLVM_ASYNC_UNEXPECTED_STDERR' >&2
  cat build/atto-tenant-full-source/m5-llvm-stderr >&2
  exit 1
}
for marker in \
  M5_LLVM_MPSC_ACCEPTED=8000 \
  M5_LLVM_NEIGHBOR_ACCEPTED=200 \
  M5_LLVM_NEIGHBOR_STATE=400 \
  M5_LLVM_ACTOR_STATE=8000 \
  M5_LLVM_ACTOR_RESTARTS=1 \
  M5_LLVM_UNEXPECTED_PRODUCER_ERRORS=0 \
  M5_LLVM_POST_DRAIN_QUEUED_BYTES=0 \
  M5_LLVM_NATIVE_ASYNC_MAILBOX_HOST=PASSED; do
  grep -Fx "$marker" build/atto-tenant-full-source/m5-llvm-output >/dev/null || {
    echo "M5_LLVM_ASYNC_MISSING_OBSERVATION=$marker" >&2
    exit 1
  }
done
echo "M5_LLVM_ASYNC_HOST_EXECUTABLE_SHA256=$(sha256sum build/atto-tenant-full-source/m5-llvm-mailbox | cut -d ' ' -f 1)"
echo 'M5_LLVM_ASYNC_MAILBOX_NATIVE_HOST=PASSED'
echo 'EVIDENCE_SCOPE=ACTUAL_LLVM_NATIVE_ASYNC_HOST_C_TEST_MODULE_NOT_COMPILED_ATTO_GUEST'
