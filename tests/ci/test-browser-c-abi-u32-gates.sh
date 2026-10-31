#!/usr/bin/env bash
# Synthetic host-only negative tests inside the PRIVATE LLVM source checkout.
# No guest execution, and NOT a source-free cross-organization transfer.
set -euo pipefail
export LC_ALL=C
root="$(cd "$(dirname "$0")/../.." && pwd -P)"
cd "$root"
# The acceptance script itself is source-reviewed only when a runner is
# admitted. Detect malformed/duplicated test tails before building a compiler.
bash -n scripts/verify-browser-c-abi-u32.sh
[[ "$(grep -c '^for fixture in ' scripts/verify-browser-c-abi-u32.sh)" == 1 ]] ||
  { echo 'BROKEN_BROWSER_U32_GATE: duplicate negative fixture loop' >&2; exit 1; }
[[ "$(grep -c "^printf 'REAL_ORESLANG_C_U32_ABI_ACCEPTANCE" scripts/verify-browser-c-abi-u32.sh)" == 1 ]] ||
  { echo 'BROKEN_BROWSER_U32_GATE: duplicate or missing acceptance marker' >&2; exit 1; }
[[ "$(grep -c '^nm -D --defined-only ' scripts/verify-browser-c-abi-u32.sh)" == 1 ]] ||
  { echo 'BROKEN_BROWSER_U32_GATE: duplicate or missing symbol audit' >&2; exit 1; }
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
# The gate must reject scripts, paths that are not absolute, and symlinks.
ln -s "$tmp/fake-compiler" "$tmp/symlink-compiler"
expect_fail symlink_compiler EXPECTED_SOURCE_SHA="$head" EXPECTED_SOURCE_TREE="$tree" \
  ORESLANG_LLVMC="$tmp/symlink-compiler" ORESLANG_COMPILER_SHA256="$sha"
expect_fail relative_compiler EXPECTED_SOURCE_SHA="$head" EXPECTED_SOURCE_TREE="$tree" \
  ORESLANG_LLVMC="fake-compiler" ORESLANG_COMPILER_SHA256="$sha"
# A modified checkout must not be accepted even with matching HEAD/tree pins.
dirty="$root/.ores-u32-negative-dirty.$BASHPID"
[[ ! -e "$dirty" ]] || { echo "unsafe dirty probe path exists" >&2; exit 1; }
trap 'rm -f -- "$dirty"; rm -rf -- "$tmp"' EXIT HUP INT TERM
printf '%s\n' 'untracked synthetic test artifact' > "$dirty"
expect_fail dirty_checkout EXPECTED_SOURCE_SHA="$head" EXPECTED_SOURCE_TREE="$tree" \
  ORESLANG_LLVMC="$tmp/fake-compiler" ORESLANG_COMPILER_SHA256="$sha"
rm -f -- "$dirty"
echo 'SYNTHETIC_HOST_ONLY_U32_CI_NEGATIVE_GATES_PASS_NOT_GUEST_EXECUTION'
