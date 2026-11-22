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
grep -Fqx 'ATTO_LLVM_NATIVE_INVENTORY_ONLY positive=38 negative=5 (NO_COMPILER_EXECUTION)' \
  "$tmp/positive.log" || fail 'good inventory did not pass'
reject() {
  label="$1"
  if ATTO_NATIVE_PARITY_MANIFEST="$tmp/cases.tsv" \
      ATTO_NATIVE_NEGATIVE_MANIFEST="$tmp/negative.tsv" \
      bash "$runner" --inventory-only > "$tmp/result.log" 2>&1; then
    fail "accepted corrupted manifest: $label"
  fi
}
cp "$cases" "$tmp/cases.tsv"
cp "$negatives" "$tmp/negative.tsv"
sed -i 's/^arith-add|/arith-subtract|/' "$tmp/cases.tsv"
reject 'duplicate positive case ID'
cp "$cases" "$tmp/cases.tsv"
sed -i 's#positive/arith-add.atto#positive/../negative/invalid-syntax.atto#' "$tmp/cases.tsv"
reject 'directory traversal source'
cp "$cases" "$tmp/cases.tsv"
sed -i 's#^arith-add|positive/arith-add.atto|42#arith-add|positive/arith-add.atto|999#' "$tmp/cases.tsv"
reject 'out-of-range native exit status'
cp "$cases" "$tmp/cases.tsv"
sed -i '/^arith-add|/d' "$tmp/cases.tsv"
reject 'missing positive'
cp "$cases" "$tmp/cases.tsv"
grep '^arith-add|' "$cases" >> "$tmp/cases.tsv"
reject 'extra positive'
cp "$cases" "$tmp/cases.tsv"
sed -i '/^unknown-function|/d' "$tmp/negative.tsv"
reject 'missing negative'
cp "$negatives" "$tmp/negative.tsv"
sed -i 's#negative/unknown-function.atto#negative/../positive/arith-add.atto#' "$tmp/negative.tsv"
reject 'negative traversal'
cp "$negatives" "$tmp/negative.tsv"
if bash "$runner" --unknown-option > "$tmp/result.log" 2>&1; then
  fail 'unknown runner mode was accepted'
fi
python3 "$root/tests/verify_atto_guest_receipts.py" --self-test
echo 'PASS: eight source-free AttoLang LLVM inventory admission mutations (NO_GUEST_EXECUTION)'
