#!/usr/bin/env bash
# Source-free mutation checks for LLVM's AttoLang scalar guest inventory.
# Never credit this as compiler-backed execution or Java TestCase parity.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
runner="$root/tests/atto_native_harness_parity.sh"
cases="$root/tests/atto-native-harness/cases.tsv"
negatives="$root/tests/atto-native-harness/negative.tsv"
tmp="$(mktemp -d)"
trap 'rm -rf -- "$tmp"' EXIT

fail() { echo "FAIL source-free AttoLang LLVM inventory: $*" >&2; exit 1; }
bash "$runner" --inventory-only > "$tmp/positive.log"
grep -Fqx 'ATTO_LLVM_NATIVE_INVENTORY_ONLY positive=40 negative=6 (NO_COMPILER_EXECUTION)' \
  "$tmp/positive.log" || fail 'good inventory did not pass'
reject() {
  label="$1"
  if ATTO_NATIVE_PARITY_MANIFEST="$tmp/cases.tsv" \
      ATTO_NATIVE_NEGATIVE_MANIFEST="$tmp/negative.tsv" \
      bash "$runner" --inventory-only > "$tmp/result.log" 2>&1; then
    fail "accepted corrupted manifest: $label"
  fi
}
reset_manifests() {
  cp "$cases" "$tmp/cases.tsv"
  cp "$negatives" "$tmp/negative.tsv"
}
reset_manifests
sed -i 's/^arith-add|/arith-subtract|/' "$tmp/cases.tsv"
reject 'duplicate positive case ID'
reset_manifests
sed -i 's#^arith-subtract|positive/arith-subtract.atto#arith-subtract|positive/arith-add.atto#' "$tmp/cases.tsv"
reject 'duplicate positive source path'
reset_manifests
sed -i 's#positive/arith-add.atto#positive/../negative/invalid-syntax.atto#' "$tmp/cases.tsv"
reject 'directory traversal source'
reset_manifests
sed -i 's#^arith-add|positive/arith-add.atto|42#arith-add|positive/arith-add.atto|999#' "$tmp/cases.tsv"
reject 'out-of-range native exit status'
reset_manifests
sed -i 's#^arith-add|positive/arith-add.atto|42$#arith-add|positive/arith-add.atto|42|#' "$tmp/cases.tsv"
grep -Fqx 'arith-add|positive/arith-add.atto|42|' "$tmp/cases.tsv" ||
  fail 'ineffective positive trailing-column mutation'
reject 'positive manifest trailing empty field'
reset_manifests
sed -i 's#^arith-add|positive/arith-add.atto|42#arith-add|positive/arith-add.atto|139#' "$tmp/cases.tsv"
reject 'signal-style native exit cannot be a success oracle'
reset_manifests
sed -i 's#^arith-add|positive/arith-add.atto|42#arith-add|positive/arith-add.atto|124#' "$tmp/cases.tsv"
reject 'timeout-status native exit cannot be a success oracle'
reset_manifests
sed -i '/^arith-add|/d' "$tmp/cases.tsv"
reject 'missing positive'
reset_manifests
grep '^arith-add|' "$cases" >> "$tmp/cases.tsv"
reject 'extra positive'
reset_manifests
sed -i '/^unknown-function|/d' "$tmp/negative.tsv"
reject 'missing negative'
reset_manifests
sed -i 's#negative/unknown-function.atto#negative/../positive/arith-add.atto#' "$tmp/negative.tsv"
reject 'negative traversal'
reset_manifests
sed -i 's#^unknown-function|negative/unknown-function.atto|unknown function$#unknown-function|negative/unknown-function.atto|unknown function|#' "$tmp/negative.tsv"
grep -Fqx 'unknown-function|negative/unknown-function.atto|unknown function|' "$tmp/negative.tsv" ||
  fail 'ineffective negative trailing-column mutation'
reject 'negative manifest trailing empty field'
reset_manifests
sed -i 's#^invalid-syntax|negative/invalid-syntax.atto#invalid-syntax|negative/unknown-function.atto#' "$tmp/negative.tsv"
reject 'duplicate negative source path'
reset_manifests
sed -i '/^std-alias-attempt|/d' "$tmp/negative.tsv"
reject 'missing reserved std negative'
reset_manifests
if bash "$runner" --unknown-option > "$tmp/result.log" 2>&1; then
  fail 'unknown runner mode was accepted'
fi
python3 "$root/tests/verify_atto_guest_receipts.py" --self-test
echo 'PASS: fifteen source-free AttoLang LLVM inventory admission mutations (NO_GUEST_EXECUTION)'
