#!/usr/bin/env bash
set -Eeuo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT
VERSION="2026.08.11.125311"
EXPECTED_SHA256="ab323b79802aedc3203b3f9af37c6aca3ed43f4e0225b36f2aa77b26de46c05f"
URL="https://github.com/zactionsz/tla-tools/releases/download/tla2tools-${VERSION}/tla2tools.jar"
curl --fail --location --retry 3 --silent --show-error -o "${TMP}/tla2tools.jar" "${URL}"
echo "${EXPECTED_SHA256}  ${TMP}/tla2tools.jar" | sha256sum --check --status
cd "${HERE}"
java -Xmx1200m -cp "${TMP}/tla2tools.jar" tlc2.TLC -workers 2 -deadlock -config LoanIdentity.cfg -metadir "${TMP}/safe" LoanIdentity.tla 2>&1 | tee "${TMP}/safe.log"
grep -Eq 'Model checking completed|Model checking finished|Finished computing' "${TMP}/safe.log"
for variant in Collision Release; do
  case "${variant}" in Collision) inv="NoAliasedAccepted";; Release) inv="CorrectRelease";; esac
  if java -Xmx1200m -cp "${TMP}/tla2tools.jar" tlc2.TLC -workers 2 -deadlock -config "LoanIdentity${variant}Mutant.cfg" -metadir "${TMP}/${variant}" LoanIdentity.tla > "${TMP}/${variant}.log" 2>&1; then
    cat "${TMP}/${variant}.log"; echo "Unsafe mutant passed" >&2; exit 1
  fi
  if ! grep -Eq "Invariant ${inv} is violated|${inv}.*violated" "${TMP}/${variant}.log"; then
    cat "${TMP}/${variant}.log"; echo "Incorrect mutant failure" >&2; exit 1
  fi
  echo "EXPECTED_MUTANT_COUNTEREXAMPLE: ${inv}"
done
echo LOAN_IDENTITY_TLC_PROOF_GATE_PASS
