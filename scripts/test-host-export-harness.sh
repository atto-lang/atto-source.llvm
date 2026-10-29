#!/usr/bin/env bash
# Self-contained negative tests of the C ABI host probe using SYNTHETIC IR.
# This is not the .ores compiler, and MUST NOT be counted as guest execution.
set -euo pipefail
export LC_ALL=C
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
cd "$root"
command -v clang >/dev/null || { echo 'NOT_EXECUTED: clang missing' >&2; exit 1; }
command -v cc >/dev/null || { echo 'NOT_EXECUTED: cc missing' >&2; exit 1; }
tmp="$(mktemp -d)"
trap 'rm -rf -- "$tmp"' EXIT HUP INT TERM
chmod 700 "$tmp"
cc -std=c11 -O2 -Wall -Wextra -Werror -pedantic \
    tests/browser_aot_scalar_export_host.c -ldl -o "$tmp/host"
cat > "$tmp/good.ll" <<'IR'
; SYNTHETIC HOST HARNESS TEST ONLY - NOT ORESLANG SOURCE
source_filename = "synthetic-positive-harness-only"
define i64 @ores_browser_aot_scalar_probe() {
entry:
  ret i64 20261029
}
IR
cat > "$tmp/wrong.ll" <<'IR'
; SYNTHETIC HOST HARNESS TEST ONLY - NOT ORESLANG SOURCE
source_filename = "synthetic-negative-result-only"
define i64 @ores_browser_aot_scalar_probe() {
entry:
  ret i64 20261028
}
IR
cat > "$tmp/forbidden.ll" <<'IR'
; SYNTHETIC HOST HARNESS TEST ONLY - NOT ORESLANG SOURCE
source_filename = "synthetic-negative-forbidden-export-only"
define i64 @ores_browser_aot_scalar_probe() {
entry:
  ret i64 20261029
}
define i32 @ores_browser_abi_version() {
entry:
  ret i32 1
}
IR
cat > "$tmp/missing.ll" <<'IR'
; SYNTHETIC HOST HARNESS TEST ONLY - NOT ORESLANG SOURCE
source_filename = "synthetic-negative-missing-export-only"
define i64 @unrelated_scalar() {
entry:
  ret i64 20261029
}
IR
for name in good wrong forbidden missing; do
    clang -O2 -fPIC -shared -x ir "$tmp/$name.ll" -o "$tmp/$name.so"
done
"$tmp/host" "$tmp/good.so" > "$tmp/good.out"
grep -Fxq 'NATIVE_ORESLANG_AOT_SCALAR_EXPORT_PASS_NOT_BROWSER_ABI' "$tmp/good.out"
expect_fail() {
    local description="$1"
    shift
    if "$tmp/host" "$@" > "$tmp/negative.out" 2> "$tmp/negative.err"; then
        echo "HOST_PROBE_FALSE_ACCEPT: $description" >&2
        exit 1
    fi
}
expect_fail 'wrong return result' "$tmp/wrong.so"
expect_fail 'forbidden browser export' "$tmp/forbidden.so"
expect_fail 'missing scalar symbol' "$tmp/missing.so"
expect_fail 'nonabsolute library path' "nonabsolute.so"
printf 'not a shared library\n' > "$tmp/garbage.so"
expect_fail 'corrupt shared library' "$tmp/garbage.so"
echo 'HOST_PROBE_SYNTHETIC_NEGATIVE_TESTS_PASS (NOT ORESLANG COMPILATION)'
