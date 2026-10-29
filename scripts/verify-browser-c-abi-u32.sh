#!/usr/bin/env bash
# Compiles real Oreslang -> LLVM -> ELF shared library -> exact uint32_t C host.
# This P0 test is NOT a browser session, actor-state, Qt or sandbox proof.
set -euo pipefail
export LC_ALL=C
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
cd "$root"
fail() { echo "BROWSER_C_U32_NOT_VERIFIED: $*" >&2; exit 1; }
[[ "$(uname -s)" == "Linux" ]] || fail "Linux ELF required"
for tool in git clang cc nm readelf timeout; do
    command -v "$tool" >/dev/null || fail "$tool unavailable"
done
head="$(git rev-parse --verify HEAD)" || fail "source HEAD unavailable"
tree="$(git rev-parse --verify 'HEAD^{tree}')" || fail "source tree unavailable"
[[ -n "${EXPECTED_SOURCE_SHA:-}" && "$EXPECTED_SOURCE_SHA" == "$head" ]] ||
    fail "exact checkout SHA mismatch"
[[ -z "$(git status --porcelain --untracked-files=all)" ]] || fail "dirty source checkout"
compiler="${ORESLANG_LLVMC:-}"
[[ "$compiler" == /* && -f "$compiler" && -x "$compiler" && ! -L "$compiler" ]] ||
    fail "absolute native executable compiler required"
readelf -h "$compiler" >/dev/null 2>&1 || fail "compiler is not a native ELF binary"
case "${ORESLANG_COMPILER_SHA256:-}" in
    "" | *[!0123456789abcdef]*) fail "invalid compiler SHA256 pin" ;;
esac
[[ "${#ORESLANG_COMPILER_SHA256}" -eq 64 ]] || fail "compiler SHA256 length"
digest="$(sha256sum "$compiler" | cut -d ' ' -f1)"
[[ "$digest" == "$ORESLANG_COMPILER_SHA256" ]] || fail "compiler binary pin mismatch"
work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT HUP INT TERM
chmod 700 "$work"
timeout --kill-after=5s 45s "$compiler" --target=host \
    tests/fixtures/browser-c-abi-version-u32.ores > "$work/version.ll" ||
    fail "native Oreslang compiler rejected valid ABI source"
[[ "$(grep -Ec '^define (dso_local )?i32 @ores_browser_abi_version\(' "$work/version.ll")" == 1 ]] ||
    fail "exact i32 C ABI export absent or duplicated"
[[ "$(grep -Ec '^define (dso_local )?i64 @browser_native_abi_v1\(' "$work/version.ll")" == 1 ]] ||
    fail "expected guest i64 symbol absent or duplicated"
! grep -Eq '^define .*@ores_browser_(open|dispatch|snapshot|close)\(' "$work/version.ll" ||
    fail "unexpected browser session C ABI symbol"
timeout --kill-after=5s 45s clang -O2 -fPIC -shared \
    -Wl,-z,defs -Wl,-z,relro,-z,now -x ir "$work/version.ll" \
    -o "$work/version.so" || fail "native ELF link failed"
readelf -h "$work/version.so" | grep -q 'DYN' ||
    fail "AOT output is not a shared ELF object"
nm -D --defined-only "$work/version.so" > "$work/exports" ||
    fail "ELF dynamic symbols not inspectable"
[[ "$(grep -Ec ' [TW] ores_browser_abi_version$' "$work/exports")" == 1 ]] ||
    fail "compiled C ABI symbol not defined in ELF"
! grep -Eq ' ores_browser_(open|dispatch|snapshot|close)$' "$work/exports" ||
    fail "unexpected exported session ABI"
cc -std=c11 -O2 -Wall -Wextra -Werror -pedantic \
    tests/browser_c_abi_u32_host.c -ldl -o "$work/c-host"
timeout --kill-after=5s 15s "$work/c-host" "$work/version.so" ||
    fail "compiled Oreslang-to-C ABI runtime failed"
for fixture in \
    browser-c-abi-version-u32-bad-symbol \
    browser-c-abi-version-u32-private \
    browser-c-abi-version-u32-bad-value \
    browser-c-abi-version-u32-bad-signature \
    browser-c-abi-version-u32-bad-type \
    browser-c-abi-version-u32-symbol-clash \
    browser-c-abi-version-u32-effectful \
    browser-c-abi-version-u32-duplicate
do
    if timeout --kill-after=5s 20s "$compiler" --target=host \
        "tests/fixtures/$fixture.ores" > "$work/rejected.ll" 2>"$work/rejected.err"; then
        fail "compiler wrongly accepted $fixture"
    fi
    [[ ! -s "$work/rejected.ll" ]] ||
        fail "compiler emitted IR for rejected source $fixture"
done
echo "REAL_ORESLANG_C_U32_ABI_NEGATIVE_TESTS_PASS"
printf 'REAL_ORESLANG_C_U32_ABI_ACCEPTANCE source_sha=%s tree_sha=%s compiler_sha256=%s\n' \
    "$head" "$tree" "$digest"
