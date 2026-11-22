#!/usr/bin/env bash
# Tests ONLY the exact-checkout script's provenance, negative and toolchain
# contract using synthetic source. Never claim actual OresVM or .atto guest pass.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
gate="$root/tests/tenantvm_actor_turn_exact_checkout.sh"
[[ -f "$gate" ]]
tmp="$(mktemp -d)"
trap 'rm -rf -- "$tmp"' EXIT
fixture="$tmp/synthetic-test-repo"
mkdir -p "$fixture/tests" "$fixture/src/main/cpp/oreslang/runtime"
cp "$gate" "$fixture/tests/tenantvm_actor_turn_exact_checkout.sh"
for name in OresVM.hpp OresIsolateVM.hpp OresIsolateAbi.h; do
  printf '#pragma once\n' > "$fixture/src/main/cpp/oreslang/runtime/$name"
done
cat > "$fixture/tests/native_tenantvm_actor_turn_object_test.cpp" <<'CPP'
#include <atomic>
#include <cstdio>
#include <thread>
int main() {
    static thread_local int marker = 17;
    std::atomic<bool> good{false};
    std::thread other([&] { good.store(marker == 17); });
    other.join();
    if (!good.load()) return 1;
    std::puts("SOURCE_FREE_FIXTURE_EXECUTED");
    return 0;
}
CPP
git -C "$fixture" init -q
git -C "$fixture" config user.email gate-fixture@example.invalid
git -C "$fixture" config user.name 'source-free gate fixture'
git -C "$fixture" add .
git -C "$fixture" commit -qm 'purely synthetic security gate test'
head="$(git -C "$fixture" rev-parse HEAD)"
cd "$fixture"
for compiler in g++ clang++; do
  for mode in optimized asan-ubsan; do
    bash tests/tenantvm_actor_turn_exact_checkout.sh \
      "$head" "$compiler" "$mode" >"$tmp/out"
    grep -Fq 'SOURCE_FREE_FIXTURE_EXECUTED' "$tmp/out"
    grep -Fq "EXACT_HEAD_TENANTVM_ACTOR_TURN_HOST_PASS head=$head" "$tmp/out"
  done
done
expect_denial() {
  if bash tests/tenantvm_actor_turn_exact_checkout.sh "$@" >"$tmp/denied.out" 2>&1; then
    echo "FAIL: exact-checkout gate accepted intentionally invalid case: $*" >&2
    exit 1
  fi
  if grep -Fq 'EXACT_HEAD_TENANTVM_ACTOR_TURN_HOST_PASS' "$tmp/denied.out"; then
    echo 'FAIL: denied case emitted a success receipt' >&2
    exit 1
  fi
}
expect_denial '0000000000000000000000000000000000000000' g++ optimized
expect_denial "$head" g++ nonexistent-mode
expect_denial "$head" unsupported-compiler optimized
printf 'dirty\n' >> src/main/cpp/oreslang/runtime/OresVM.hpp
expect_denial "$head" g++ optimized
git checkout -q -- src/main/cpp/oreslang/runtime/OresVM.hpp
printf 'untracked\n' > unexpected.txt
expect_denial "$head" g++ optimized
rm unexpected.txt
rm src/main/cpp/oreslang/runtime/OresVM.hpp
ln -s OresIsolateVM.hpp src/main/cpp/oreslang/runtime/OresVM.hpp
expect_denial "$head" g++ optimized
printf 'SOURCE_FREE_EXACT_CHECKOUT_GATE_SELFTEST_PASS head=%s\n' "$head"
