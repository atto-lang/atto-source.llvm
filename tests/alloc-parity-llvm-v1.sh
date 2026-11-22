#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
compiler="${1:-./build/oreslang-llvmc}"
test -x "$compiler" || { echo "missing real LLVM guest compiler $compiler" >&2; exit 1; }
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

# Existing strict compiler -> emitted LLVM IR -> clang linked native executable
# oracle validates new/read/drop/close call sites, exit 42, 10 ownership
# compiler negatives and unsupported target fail-closed gates.
bash tests/atto_box_main_v1.sh "$compiler" 2>&1 | tee "$tmp/box.log"
grep -Fqx 'ATTO_MAIN_BOX_I64_AOT_PASS' "$tmp/box.log"
grep -Fqx 'ATTO_BOX_MAIN_V1_NEGATIVE_10_EXPLICIT_DIAGNOSTICS_PASS' "$tmp/box.log"

count=0
while IFS=$'\t' read -r case_id phase outcome path; do
  [[ -z "$case_id" || "$case_id" == \#* ]] && continue
  test "$phase" = "guest" && test "$outcome" = "PASS"
  case "$case_id:$path" in
    lexical.normal_return_once:tests/atto-box-main-v1/return-get.atto|\
    lexical.early_return_once:tests/atto-box-main-v1/return-early.atto) ;;
    *) echo "unapproved LLVM lexical guest proof mapping" >&2; exit 1 ;;
  esac
  test -f "$path"
  blob="$(git hash-object "$path")"
  test "$(git rev-parse "HEAD:$path")" = "$blob"
  printf 'ATTO_ALLOC_CASE %s %s %s %s\n' "$case_id" "$phase" "$outcome" "$blob"
  count=$((count+1))
done < tests/alloc-parity-llvm-v1.tsv
test "$count" -eq 2
echo ATTO_LLVM_ALLOC_LEXICAL_2_AOT_EXECUTED
# Intentionally omit ATTO_ALLOC_GUEST_EXECUTED: it would imply complete
# shared-contract coverage, including cases not implemented by LLVM yet.
