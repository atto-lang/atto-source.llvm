#!/usr/bin/env bash
# Exact-head LLVM O0/O2 compiled-guest gate for core-four.
# Source-only and synthetic compiler results NEVER satisfy this script.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd -P)"
cd "$root"
: "$EXPECTED_HEAD"
[[ -n "$EXPECTED_HEAD" && "$(git rev-parse HEAD)" == "$EXPECTED_HEAD" ]] || {
  echo 'core-four: checkout SHA mismatch' >&2; exit 65;
}
[[ -z "$(git status --porcelain --untracked-files=no)" ]] || {
  echo 'core-four: dirty tracked source' >&2; exit 65;
}
compiler="$root/build/oreslang-llvmc"
auditor="$root/build/oreslang-no-gc-ir-audit"
[[ -x "$compiler" && -x "$auditor" ]] || {
  echo 'core-four: compiled llvmc and no-GC IR auditor required' >&2; exit 69;
}
command -v clang >/dev/null || { echo 'core-four: clang required' >&2; exit 69; }
command -v node >/dev/null || { echo 'core-four: node required' >&2; exit 69; }
tmp="$(mktemp -d)"
trap 'rm -rf -- "$tmp"' EXIT
export ORESLANG_LLVMC="$compiler" ORES_NO_GC_IR_AUDIT="$auditor"
export ATTO_CORE_FOUR_LANE=llvm-native
export ATTO_CORE_FOUR_COMPILE_ARGV_JSON='["bash","scripts/compile-aot.sh","{source}","{artifact}"]'
export ATTO_CORE_FOUR_RUN_ARGV_JSON='["{artifact}"]'
for level in 0 2; do
  export ORESLANG_LLVM_AOT_OPT_LEVEL="$level"
  node scripts/run-core-four-guest.mjs > "$tmp/receipt-O$level.json"
  node -e '
    const fs=require("node:fs");
    const x=JSON.parse(fs.readFileSync(process.argv[1],"utf8"));
    if(x.failed!==0 || x.passed!==2 || x.certified_guest_passes!==0)
      process.exit(1);
    if(x.observations.some(o=>o.status!=="local-executed-observation"))
      process.exit(1);
  ' "$tmp/receipt-O$level.json" || {
    echo "core-four: O$level compiled guest receipts incomplete" >&2; exit 1;
  }
  printf 'CORE_FOUR_LLVM_LOCAL_GUEST_PASS|head=%s|O%s|fixtures=2|compilation=real|source_model=no\n' "$EXPECTED_HEAD" "$level"
done
echo 'CORE_FOUR_LLVM_LOCAL_MATRIX_PASS|O0+O2|non_certifying_until_independent_CI'
