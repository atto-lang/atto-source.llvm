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
# Probe failure is deliberate; successful cases MUST emit native ELF.
# A shell-script fake can no longer pass linked-image admission.
for arg in "$@"; do
  if [[ "$arg" == */lto-main.ll && "${MOCK_PROBE_EXIT:-0}" == 1 ]]; then
    exit 1
  fi
done
for arg in "$@"; do
  if [[ "$arg" == */program.ll ]]; then
    printf '%s\n' "$@" > "$MOCK_FINAL_LOG"
    break
  fi
done
exec "$REAL_CLANG" "$@"
EOF
chmod +x "$work/mock-compiler" "$work/mock-clang"
export REAL_CLANG="$(command -v clang)"
export MOCK_FINAL_LOG="$work/final.log"
export MOCK_CLANG_LOG="$work/clang.log"
cat >"$work/mock-auditor" <<'EOF'
#!/usr/bin/env bash
test -s "$1"
EOF
chmod +x "$work/mock-auditor"
export ORES_NO_GC_IR_AUDIT="$work/mock-auditor"
export CLANG="$work/mock-clang"
export ORESLANG_LLVMC="$work/mock-compiler"
printf 'previous executable\n' >"$work/app"
# Baseline must not request LTO.
: >"$MOCK_CLANG_LOG"
ORESLANG_AOT_LTO=off bash "$root/scripts/compile-aot.sh" "$work/input.ores" "$work/app"
! grep -q -- '-flto' "$MOCK_CLANG_LOG"
! grep -q -- '-flto' "$MOCK_FINAL_LOG"
# Full and Thin modes run an executable two-unit LLVM IR canary plus final IR link.
for mode in full thin; do
  : >"$MOCK_CLANG_LOG"
  ORESLANG_AOT_LTO="$mode" bash "$root/scripts/compile-aot.sh" "$work/input.ores" "$work/app"
  if [[ "$mode" == full ]]; then
    grep -qx -- '-flto' "$MOCK_FINAL_LOG"
  else
    grep -qx -- '-flto=thin' "$MOCK_FINAL_LOG"
  fi
  grep -qx -- '-fuse-ld=lld' "$MOCK_FINAL_LOG"
  grep -q -- '/lto-helper.ll' "$MOCK_CLANG_LOG"
  grep -q -- '/lto-main.ll' "$MOCK_CLANG_LOG"
done
# Unsupported toolchains or incorrect canary results preserve the published artifact.
printf 'preserved\n' >"$work/app"
if MOCK_FAIL_LTO=1 ORESLANG_AOT_LTO=full bash "$root/scripts/compile-aot.sh" "$work/input.ores" "$work/app"; then
  echo 'unexpected successful LTO fallback' >&2; exit 1
fi
[[ "$(cat "$work/app")" == preserved ]]
if MOCK_PROBE_EXIT=1 ORESLANG_AOT_LTO=thin bash "$root/scripts/compile-aot.sh" "$work/input.ores" "$work/app"; then
  echo 'unexpected acceptance of failed or incorrect canary' >&2; exit 1
fi
[[ "$(cat "$work/app")" == preserved ]]
if ORESLANG_AOT_LTO=bogus bash "$root/scripts/compile-aot.sh" "$work/input.ores" "$work/app"; then
  echo 'unexpected acceptance of unknown LTO mode' >&2; exit 1
fi
[[ "$(cat "$work/app")" == preserved ]]
echo 'LTO driver policy smoke passed (real Clang/LLD, synthetic IR producer; no source compiler claim).'
