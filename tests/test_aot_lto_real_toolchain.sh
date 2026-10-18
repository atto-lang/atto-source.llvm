#!/usr/bin/env bash
# Real Clang+LLD integration against a synthetic LLVM IR-producing fixture.
# This does NOT claim Oreslang source parsing, multi-module lowering or speedups.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd -P)"
work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT
clang="${CLANG:-clang}"
command -v "$clang" >/dev/null || { echo 'UNSUPPORTED: Clang missing' >&2; exit 78; }
command -v ld.lld >/dev/null || { echo 'UNSUPPORTED: LLD missing' >&2; exit 78; }
cat > "$work/synthetic-ir-producer" <<'ORES_LLVM_FIXTURE'
#!/usr/bin/env bash
set -euo pipefail
[[ "$1" == --target=host && $# == 2 ]] || exit 64
printf 'define i32 @main() { ret i32 42 }\n'
ORES_LLVM_FIXTURE
chmod +x "$work/synthetic-ir-producer"
printf 'Fixture only: not real Oreslang source.\n' >"$work/input.ores"
for mode in off full thin; do
  env CLANG="$clang" ORESLANG_LLVMC="$work/synthetic-ir-producer" ORESLANG_AOT_LTO="$mode" bash "$root/scripts/compile-aot.sh" "$work/input.ores" "$work/result"
  set +e
  "$work/result"
  result=$?
  set -e
  [[ "$result" == 42 ]] || { echo "FAIL: $mode produced status $result, expected 42" >&2; exit 1; }
  printf 'REAL_TOOLCHAIN_OK mode=%s (Clang/LLD, synthetic IR only)\n' "$mode"
done
