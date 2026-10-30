#!/usr/bin/env bash
# Synthetic negative tests for the CI identity and compiler-verification gate.
# These must never be reported as genuine Oreslang guest execution.
set -euo pipefail
export LC_ALL=C
root="$(cd "$(dirname "$0")/../.." && pwd -P)"
cd "$root"
for tool in git cc clang readelf sha256sum timeout; do
  command -v "$tool" >/dev/null 2>&1 || { echo "NOT_EXECUTED: missing $tool"; exit 1; }
done
tmp="$(mktemp -d)"
trap 'rm -rf -- "$tmp"' EXIT HUP INT TERM
chmod 700 "$tmp"
cat > "$tmp/fake.c" <<'C'
/* Deliberately WRONG compiler ELF: succeeds but produces no LLVM IR. */
int main(void) { return 0; }
C
cc -std=c11 -Wall -Wextra -Werror -pedantic "$tmp/fake.c" -o "$tmp/fake-compiler"
sha="$(sha256sum "$tmp/fake-compiler" | cut -d ' ' -f1)"
head="$(git rev-parse --verify HEAD)"
tree="$(git rev-parse --verify 'HEAD^{tree}')"
bad_sha=0000000000000000000000000000000000000000
bad_digest=0000000000000000000000000000000000000000000000000000000000000000
expect_fail() {
  local scenario="$1"
  shift
  if env "$@" bash scripts/verify-browser-c-abi-u32.sh > "$tmp/out" 2> "$tmp/err"; then
    echo "FALSE_ACCEPTED: $scenario" >&2
    exit 1
  fi
  grep -Fq 'BROWSER_C_U32_NOT_VERIFIED' "$tmp/err" ||
    { echo "UNCATEGORIZED_REJECTION: $scenario" >&2; exit 1; }
  ! grep -Fq 'REAL_ORESLANG_C_U32_ABI_ACCEPTANCE' "$tmp/out" ||
    { echo "FALSE_GUEST_ACCEPTANCE: $scenario" >&2; exit 1; }
}
expect_fail wrong_head EXPECTED_SOURCE_SHA="$bad_sha" EXPECTED_SOURCE_TREE="$tree" \
  ORESLANG_LLVMC="$tmp/fake-compiler" ORESLANG_COMPILER_SHA256="$sha"
expect_fail wrong_tree EXPECTED_SOURCE_SHA="$head" EXPECTED_SOURCE_TREE="$bad_sha" \
  ORESLANG_LLVMC="$tmp/fake-compiler" ORESLANG_COMPILER_SHA256="$sha"
expect_fail wrong_digest EXPECTED_SOURCE_SHA="$head" EXPECTED_SOURCE_TREE="$tree" \
  ORESLANG_LLVMC="$tmp/fake-compiler" ORESLANG_COMPILER_SHA256="$bad_digest"
expect_fail fake_native_compiler EXPECTED_SOURCE_SHA="$head" EXPECTED_SOURCE_TREE="$tree" \
  ORESLANG_LLVMC="$tmp/fake-compiler" ORESLANG_COMPILER_SHA256="$sha"
echo 'SOURCE_FREE_U32_CI_NEGATIVE_GATES_PASS_NOT_GUEST_EXECUTION'
