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
  -workers 2 -deadlock -config NativeParentStopClaim.cfg \
  -metadir "$TMP/safety" NativeParentStopClaim.tla > "$TMP/safety.log" 2>&1 || {
    cat "$TMP/safety.log"; exit 1;
  }
grep -q 'Model checking completed. No error has been found.' "$TMP/safety.log" || {
    cat "$TMP/safety.log"; exit 1;
}
for config in NativeParentStopClaimLateClaimMutant NativeParentStopClaimDuplicateMutant; do
  if [ "$config" = NativeParentStopClaimLateClaimMutant ]; then
    invariant=NoOriginalClaimAfterStop
  else
    invariant=OneUseClaim
  fi
  if java -Xmx1200m -cp "$TMP/tla2tools.jar" tlc2.TLC \
       -workers 2 -deadlock -config "$config.cfg" \
       -metadir "$TMP/$config" NativeParentStopClaim.tla \
       > "$TMP/$config.log" 2>&1; then
    cat "$TMP/$config.log"; echo "UNSAFE_PARENT_CLAIM_MUTANT_PASSED" >&2; exit 1
  fi
  grep -Eq "Invariant $invariant is violated|$invariant.*violated" "$TMP/$config.log" || {
    cat "$TMP/$config.log"; echo "MUTANT_WRONG_FAILURE: $config" >&2; exit 1;
  }
  echo "EXPECTED_PARENT_STOP_CLAIM_COUNTEREXAMPLE: $config"
done
echo NATIVE_PARENT_STOP_CLAIM_TLC_PASS
