#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd -P)"
work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT
printf 'pub fnc main(): int { return 1; }\n' >"$work/input.ores"
cat >"$work/mock-compiler" <<'EOF'
#!/usr/bin/env bash
printf 'define i32 @main() { ret i32 0 }\n'
EOF
cat >"$work/mock-clang" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
printf '%s\n' "$@" >> "$MOCK_CLANG_LOG"
for arg in "$@"; do
  if [[ "${MOCK_FAIL_LTO:-0}" == 1 && "$arg" == -flto* ]]; then
    echo 'mock unsupported LTO' >&2
    exit 1
  fi
done
output=
while [[ $# -gt 0 ]]; do
  if [[ "$1" == "-o" ]]; then shift; output="$1"; fi
  shift
done
[[ -n "$output" ]] || exit 2
printf 'fake executable\n' >"$output"
EOF
chmod +x "$work/mock-compiler" "$work/mock-clang"
export MOCK_CLANG_LOG="$work/clang.log"
export CLANG="$work/mock-clang"
export ORESLANG_LLVMC="$work/mock-compiler"
printf 'previous executable\n' >"$work/app"
# Baseline must not request LTO.
: >"$MOCK_CLANG_LOG"
ORESLANG_AOT_LTO=off "$root/scripts/compile-aot.sh" "$work/input.ores" "$work/app"
! grep -q -- '-flto' "$MOCK_CLANG_LOG"
# Full and Thin modes run a toolchain probe plus final IR link.
for mode in full thin; do
  : >"$MOCK_CLANG_LOG"
  ORESLANG_AOT_LTO="$mode" "$root/scripts/compile-aot.sh" "$work/input.ores" "$work/app"
  if [[ "$mode" == full ]]; then
    grep -qx -- '-flto' "$MOCK_CLANG_LOG"
  else
    grep -qx -- '-flto=thin' "$MOCK_CLANG_LOG"
  fi
  grep -qx -- '-fuse-ld=lld' "$MOCK_CLANG_LOG"
done
# An unsupported option fails before publication, preserving existing output.
printf 'preserved\n' >"$work/app"
if MOCK_FAIL_LTO=1 ORESLANG_AOT_LTO=full "$root/scripts/compile-aot.sh" "$work/input.ores" "$work/app"; then
  echo 'unexpected successful LTO fallback' >&2; exit 1
fi
[[ "$(cat "$work/app")" == preserved ]]
if ORESLANG_AOT_LTO=bogus "$root/scripts/compile-aot.sh" "$work/input.ores" "$work/app"; then
  echo 'unexpected acceptance of unknown LTO mode' >&2; exit 1
fi
[[ "$(cat "$work/app")" == preserved ]]
echo 'LTO driver policy smoke passed (mock linker only; no real LTO claim).'
