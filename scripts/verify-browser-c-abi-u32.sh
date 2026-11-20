#!/usr/bin/env bash
# Real Oreslang -> LLVM IR -> native ELF -> exact-width C host acceptance.
# Does not test browser sessions, actor state, Qt, sandboxing or other exports.
set -euo pipefail
export LC_ALL=C

root="$(cd "$(dirname "$0")/.." && pwd -P)"
cd "$root"
fail() { printf 'BROWSER_C_U32_NOT_VERIFIED: %s\n' "$*" >&2; exit 1; }
[[ "$(uname -s)" == "Linux" ]] || fail "Linux ELF is required"
for tool in git clang cc nm readelf sha256sum timeout awk; do
  command -v "$tool" >/dev/null 2>&1 || fail "$tool unavailable"
done
[[ -v EXPECTED_SOURCE_SHA && "$EXPECTED_SOURCE_SHA" =~ ^[0-9a-f]{40}$ ]] ||
  fail "40-character immutable source commit is required"
[[ -v EXPECTED_SOURCE_TREE && "$EXPECTED_SOURCE_TREE" =~ ^[0-9a-f]{40}$ ]] ||
  fail "40-character immutable source tree is required"
head="$(git rev-parse --verify HEAD)" || fail "source HEAD unavailable"
tree="$(git rev-parse --verify 'HEAD^{tree}')" || fail "source tree unavailable"
[[ "$head" == "$EXPECTED_SOURCE_SHA" ]] || fail "exact source checkout SHA mismatch"
[[ "$tree" == "$EXPECTED_SOURCE_TREE" ]] || fail "exact source Git tree mismatch"
[[ -z "$(git status --porcelain --untracked-files=all)" ]] ||
  fail "source checkout is not clean"
[[ -v ORESLANG_LLVMC ]] || fail "compiler pathname missing"
compiler="$ORESLANG_LLVMC"
[[ "$compiler" == /* && -f "$compiler" && -x "$compiler" && ! -L "$compiler" ]] ||
  fail "absolute, regular executable non-symlink compiler required"
readelf -h "$compiler" >/dev/null 2>&1 ||
  fail "compiler is not a native ELF binary"
[[ -v ORESLANG_COMPILER_SHA256 && "$ORESLANG_COMPILER_SHA256" =~ ^[0-9a-f]{64}$ ]] ||
  fail "valid compiler SHA256 pin required"
digest="$(sha256sum "$compiler" | cut -d ' ' -f1)"
[[ "$digest" == "$ORESLANG_COMPILER_SHA256" ]] ||
  fail "compiler binary SHA256 mismatch"

work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT HUP INT TERM
chmod 700 "$work"
timeout --kill-after=5s 45s "$compiler" --target=host \
  tests/fixtures/browser-c-abi-version-u32.ores > "$work/version.ll" ||
  fail "native Oreslang compiler rejected valid browser version source"
[[ "$(grep -Ec '^define (dso_local )?i32 @ores_browser_abi_version\(' "$work/version.ll")" == 1 ]] ||
  fail "exact i32 C ABI export absent or duplicated in compiler IR"
[[ "$(grep -Ec '^define internal i64 @browser_native_abi_v1\(' "$work/version.ll")" == 1 ]] ||
  fail "private i64 guest implementation absent or duplicated in compiler IR"
! grep -Eq '^define .*@ores_browser_(open|dispatch|snapshot|close)\(' "$work/version.ll" ||
  fail "unexpected browser session export in compiler IR"

timeout --kill-after=5s 45s clang -O2 -fPIC -shared \
  -Wl,-z,defs -Wl,-z,relro,-z,now -x ir "$work/version.ll" \
  -o "$work/version.so" || fail "native ELF shared-library link failed"
readelf -h "$work/version.so" | grep -q 'DYN' ||
  fail "AOT output is not a shared ELF object"
nm -D --defined-only "$work/version.so" > "$work/exports" ||
  fail "ELF dynamic symbols unavailable"
[[ "$(grep -Ec ' [TW] ores_browser_abi_version$' "$work/exports")" == 1 ]] ||
  fail "compiled C ABI symbol not uniquely exported"
# This initial ABI must expose exactly one dynamically defined ELF symbol.
# A future session ABI must deliberately expand this oracle with real Oreslang
# compiler-backed ownership and C calling-convention proof.
awk '$3 != "ores_browser_abi_version" { bad = 1; print "UNEXPECTED_EXPORT:", $0 > "/dev/stderr" }
     END { exit bad }' "$work/exports" ||
  fail "native ELF exposed an unauthorized dynamic symbol"
! grep -Eq '[[:space:]]browser_native_abi_v1$' "$work/exports" ||
  fail "private i64 guest was exposed to dynamic linking"
! grep -Eq '[[:space:]]ores_browser_(open|dispatch|snapshot|close)$' "$work/exports" ||
  fail "unsupported browser session export is present"

cc -std=c11 -O2 -Wall -Wextra -Werror -pedantic \
  tests/browser_c_abi_u32_host.c -ldl -o "$work/c-host" ||
  fail "C11 host failed compilation"
timeout --kill-after=5s 15s "$work/c-host" "$work/version.so" ||
  fail "compiled Oreslang-to-C ABI execution failed"

for fixture in \
  browser-c-abi-version-u32-bad-symbol \
  browser-c-abi-version-u32-private \
  browser-c-abi-version-u32-bad-value \
  browser-c-abi-version-u32-bad-signature \
  browser-c-abi-version-u32-bad-type \
  browser-c-abi-version-u32-symbol-clash \
  browser-c-abi-version-u32-guest-spoof \
  browser-c-abi-version-u32-overload \
  browser-c-abi-version-u32-effectful \
  browser-c-abi-version-u32-extra-guest \
  browser-c-abi-version-u32-extra-declaration \
  browser-c-abi-version-u32-duplicate
do
  if timeout --kill-after=5s 20s "$compiler" --target=host \
    "tests/fixtures/$fixture.ores" > "$work/rejected.ll" 2> "$work/rejected.err"; then
    fail "compiler wrongly accepted $fixture"
  else
    status="$?"
    [[ "$status" != 124 && "$status" != 137 ]] ||
      fail "compiler timed out rather than rejecting $fixture"
  fi
  [[ ! -s "$work/rejected.ll" ]] ||
    fail "compiler emitted IR for rejected source $fixture"
  grep -Fq 'C_EXPORT_U32_REJECT' "$work/rejected.err" ||
    fail "fixture $fixture failed without the required rejection diagnostic"
done
echo 'REAL_ORESLANG_C_U32_ABI_NEGATIVE_TESTS_PASS'
printf 'REAL_ORESLANG_C_U32_ABI_ACCEPTANCE source_sha=%s tree_sha=%s compiler_sha256=%s\n' \
  "$head" "$tree" "$digest"
