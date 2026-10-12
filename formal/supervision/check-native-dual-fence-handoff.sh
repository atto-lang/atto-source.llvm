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
  -workers 2 -deadlock -config NativeDualFenceHandoff.cfg \
  -metadir "$TMP/safety" NativeDualFenceHandoff.tla > "$TMP/safety.log" 2>&1 || {
    cat "$TMP/safety.log"; exit 1;
  }
grep -q 'Model checking completed. No error has been found.' "$TMP/safety.log" || {
    cat "$TMP/safety.log"; exit 1;
}
for pair in \
  NativeDualFenceHandoffOwnerMutant.cfg:NoUnownedHidden \
  NativeDualFenceHandoffRegistryMutant.cfg:NoClosedCommit \
  NativeDualFenceHandoffControlMutant.cfg:NoClosedCommit; do
  cfg="$(printf '%s\n' "$pair" | cut -d: -f1)"
  invariant="$(printf '%s\n' "$pair" | cut -d: -f2)"
  name="$(basename "$cfg" .cfg)"
  if java -Xmx1200m -cp "$TMP/tla2tools.jar" tlc2.TLC \
      -workers 2 -deadlock -config "$cfg" \
      -metadir "$TMP/$name" NativeDualFenceHandoff.tla > "$TMP/$name.log" 2>&1; then
    cat "$TMP/$name.log"
    echo "UNSAFE_MUTANT_PASSED: $name" >&2
    exit 1
  fi
  grep -Eq "Invariant $invariant is violated|$invariant.*violated" "$TMP/$name.log" || {
    cat "$TMP/$name.log"
    echo "WRONG_MUTANT_FAILURE: $name" >&2
    exit 1
  }
  echo "EXPECTED_DUAL_FENCE_COUNTEREXAMPLE: $name"
done
echo NATIVE_DUAL_FENCE_HANDOFF_TLC_PASS
