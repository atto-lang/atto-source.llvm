#!/usr/bin/env bash
# First genuine native Oreslang -> LLVM IR -> AOT .so -> C dlopen boundary.
# Not a browser backend, full C ABI v1, actor-state proof or Qt integration.
set -euo pipefail
export LC_ALL=C
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
cd "$root"
fail() { printf 'NOT_EXECUTED_BROWSER_AOT_SCALAR_EXPORT: %s\n' "$*" >&2; exit 1; }
command -v git >/dev/null || fail "git unavailable"
command -v clang >/dev/null || fail "clang unavailable"
command -v cc >/dev/null || fail "C compiler unavailable"
command -v nm >/dev/null || fail "native dynamic-symbol inspection unavailable"
command -v readelf >/dev/null || fail "ELF compiler provenance inspection unavailable"
[[ "$(uname -s)" == "Linux" ]] || fail "this ELF/Clang AOT smoke requires Linux"
limiter="$(command -v timeout || command -v gtimeout || true)"
[[ -n "$limiter" ]] || fail "timeout unavailable"
head="$(git rev-parse --verify HEAD)" || fail "no source commit"
tree="$(git rev-parse --verify 'HEAD^{tree}')" || fail "no source tree"
[[ "${EXPECTED_SOURCE_SHA:-}" == "$head" ]] || fail "exact source head SHA required"
[[ -z "$(git status --porcelain --untracked-files=all)" ]] ||
    fail "dirty source checkout"
compiler="${ORESLANG_LLVMC:-}"
[[ "$compiler" == /* && -x "$compiler" && ! -L "$compiler" ]] ||
    fail "absolute regular executable ORESLANG_LLVMC required"
case "${ORESLANG_COMPILER_SHA256:-}" in
    "" | *[!0123456789abcdef]*) fail "lowercase compiler SHA256 pin required" ;;
esac
[[ "${#ORESLANG_COMPILER_SHA256}" -eq 64 ]] || fail "invalid compiler digest"
if command -v sha256sum >/dev/null; then
    digest="$(sha256sum "$compiler" | cut -d ' ' -f1)"
elif command -v shasum >/dev/null; then
    digest="$(shasum -a 256 "$compiler" | cut -d ' ' -f1)"
else
    fail "SHA256 program missing"
fi
[[ "$digest" == "$ORESLANG_COMPILER_SHA256" ]] ||
    fail "compiler digest mismatch"
readelf -h "$compiler" > /dev/null 2>&1 ||
    fail "compiler must be a native ELF binary, not a script or wrapper"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
chmod 700 "$tmp"
"$limiter" --kill-after=5s 30s "$compiler" --target=host \
    tests/fixtures/browser-aot-scalar-export.ores > "$tmp/probe.ll" ||
    fail "native Oreslang compile failed"
grep -Eq '^define (dso_local )?i64 @ores_browser_aot_scalar_probe\(' "$tmp/probe.ll" ||
    fail "compiler did not emit exact i64 guest export signature"
! grep -Eq '^define .*@ores_browser_(abi_version|open|dispatch|snapshot|close)\(' "$tmp/probe.ll" ||
    fail "canary illegally emits browser C ABI"
"$limiter" --kill-after=5s 30s clang -O2 -fPIC -shared \
    -Wl,-z,defs -Wl,-z,relro,-z,now -x ir \
    "$tmp/probe.ll" -o "$tmp/probe.so" ||
    fail "native LLVM IR shared library link failed"
nm -D --defined-only "$tmp/probe.so" > "$tmp/exports" ||
    fail "could not inspect native dynamic exports"
awk '$2 == "T" && $3 == "ores_browser_aot_scalar_probe" { n++ }
     END { exit n == 1 ? 0 : 1 }' "$tmp/exports" ||
    fail "probe not defined as exactly one shared-library text export"
! grep -Eq '[[:space:]]ores_browser_(abi_version|open|dispatch|snapshot|close)$' "$tmp/exports" ||
    fail "canary falsely defines browser ABI symbols"
cc -std=c11 -O2 -Wall -Wextra -Werror -pedantic \
    tests/browser_aot_scalar_export_host.c -ldl -o "$tmp/host"
"$limiter" --kill-after=5s 10s "$tmp/host" "$tmp/probe.so" ||
    fail "real compiled scalar export failed"
printf 'NATIVE_BROWSER_AOT_SCALAR_EXPORT_ACCEPTANCE source_sha=%s source_tree=%s compiler_sha256=%s\n' \
    "$head" "$tree" "$digest"
