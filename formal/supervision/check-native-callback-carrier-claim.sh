#!/usr/bin/env bash
set -Eeuo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
VERSION="2026.08.11.125311"
SHA="ab323b79802aedc3203b3f9af37c6aca3ed43f4e0225b36f2aa77b26de46c05f"
curl --fail --location --retry 3 --silent --show-error -o "$TMP/tla2tools.jar" \
  "https://github.com/zactionsz/tla-tools/releases/download/tla2tools-$VERSION/tla2tools.jar"
echo "$SHA  $TMP/tla2tools.jar" | sha256sum --check --status
cd "$ROOT"
java -Xmx1200m -cp "$TMP/tla2tools.jar" tlc2.TLC \
  -workers 2 -deadlock -config NativeCallbackCarrierClaim.cfg \
  -metadir "$TMP/safety" NativeCallbackCarrierClaim.tla > "$TMP/safety.log" 2>&1 || {
    cat "$TMP/safety.log"; exit 1;
  }
grep -q 'Model checking completed. No error has been found.' "$TMP/safety.log" || {
    cat "$TMP/safety.log"; exit 1;
}
for entry in \
  NativeCallbackCarrierClaimOffCarrier.cfg:NoOffCarrierClaim \
  NativeCallbackCarrierClaimLate.cfg:NoClaimAfterStop \
  NativeCallbackCarrierClaimRecycled.cfg:NoRecycledParentBorrow; do
  cfg="$(printf '%s\n' "$entry" | cut -d: -f1)"
  invariant="$(printf '%s\n' "$entry" | cut -d: -f2)"
  id="$(basename "$cfg" .cfg)"
  if java -Xmx1200m -cp "$TMP/tla2tools.jar" tlc2.TLC \
      -workers 2 -deadlock -config "$cfg" \
      -metadir "$TMP/$id" NativeCallbackCarrierClaim.tla > "$TMP/$id.log" 2>&1; then
    cat "$TMP/$id.log"
    echo "UNSAFE_CARRIER_MUTANT_PASSED: $id" >&2
    exit 1
  fi
  grep -Eq "Invariant $invariant is violated|$invariant.*violated" "$TMP/$id.log" || {
    cat "$TMP/$id.log"
    echo "UNEXPECTED_CARRIER_MUTANT_FAILURE: $id" >&2
    exit 1
  }
  echo "EXPECTED_CARRIER_COUNTEREXAMPLE: $id"
done
echo NATIVE_CALLBACK_CARRIER_TLC_PASS
