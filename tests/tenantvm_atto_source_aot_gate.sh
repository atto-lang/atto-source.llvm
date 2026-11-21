#!/usr/bin/env bash
# Compiler-backed .atto guest test, not a C++ host simulation.
# Usage: bash tests/tenantvm_atto_source_aot_gate.sh LLVM_COMPILER CLANG CLANGXX REPO_ROOT
set -euo pipefail
if [[ "$#" -ne 4 ]]; then
  echo "usage: $0 oreslang-llvmc clang clang++ repo-root" >&2
  exit 2
fi
compiler="$1"
clang="$2"
cxx="$3"
root="$4"
source_file="$root/tests/fixtures/tenantvm-trusted-scalar.atto"
harness="$root/tests/native_tenantvm_atto_guest_test.cpp"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

# The admission check happens BEFORE even reading source files. A failed
# source compilation must not leave behind a partial LLVM IR artifact.
must_reject() {
  local diagnostic="$1"
  shift
  if "$compiler" "$@" >"$tmp/rejected.ll" 2>"$tmp/denied.err"; then
    echo "UNSAFE: compiler accepted rejected tenant guest: $*" >&2
    exit 1
  fi
  if [[ -s "$tmp/rejected.ll" ]]; then
    echo "UNSAFE: rejected tenant guest emitted partial LLVM IR: $*" >&2
    exit 1
  fi
  if ! grep -Fq -- "$diagnostic" "$tmp/denied.err"; then
    echo "wrong native compiler denial for: $*" >&2
    cat "$tmp/denied.err" >&2
    exit 1
  fi
}
must_reject 'TENANTVM_UNTRUSTED_SANDBOX_UNAVAILABLE' \
  --isolate-entry=tenant_step --tenant-execution=untrusted "$source_file"
must_reject 'TENANTVM_UNTRUSTED_SANDBOX_UNAVAILABLE' \
  --isolate-entry=tenant_step --tenant-execution=untrusted \
  --target=wasm32 "$source_file"
must_reject 'TENANTVM_UNTRUSTED_SANDBOX_UNAVAILABLE' \
  --isolate-entry=tenant_step --tenant-execution=untrusted \
  "$tmp/source-file-must-not-be-read.atto"
must_reject 'tenant execution policy requires --isolate-entry' \
  --tenant-execution=trusted "$source_file"
must_reject 'duplicate tenant execution policy' \
  --tenant-execution=trusted --tenant-execution=untrusted \
  --isolate-entry=tenant_step "$source_file"
must_reject 'invalid tenant execution policy' \
  --tenant-execution=none --isolate-entry=tenant_step "$source_file"
must_reject 'tenant execution policy requires --isolate-entry' \
  --tenant-execution=untrusted "$source_file"

"$compiler" --tenant-execution=trusted --isolate-entry=tenant_step \
  "$source_file" >"$tmp/tenantvm-atto.ll" 2>"$tmp/compile.err"
test -s "$tmp/tenantvm-atto.ll"
grep -Fq 'define i32 @ores_guest_entry(' "$tmp/tenantvm-atto.ll"
grep -Fq 'llvm.sadd.with.overflow.i64' "$tmp/tenantvm-atto.ll"
grep -Fq 'isolate.arithmetic.checked' "$tmp/tenantvm-atto.ll"
grep -Fq 'ORES_GUEST' "$root/src/main/cpp/oreslang/runtime/OresIsolateAbi.h"

# LLVM IR must be produced by the ACTUAL oreslang-llvmc frontend and then
# compiled/linked as native executable code with the exact host OresVM ABI.
for level in O0 O2; do
  "$clang" -"$level" -x ir -c "$tmp/tenantvm-atto.ll" -o "$tmp/tenantvm-$level.o"
  "$cxx" -std=c++17 -"$level" -DNDEBUG -Wall -Wextra -Wpedantic -Werror \
    -pthread -I"$root/src/main/cpp" "$harness" "$tmp/tenantvm-$level.o" \
    -o "$tmp/tenantvm-$level"
  "$tmp/tenantvm-$level"
done
echo 'REAL_ATTO_LLVM_TENANTVM_AOT_EXECUTED: O0/O2, trusted only'
