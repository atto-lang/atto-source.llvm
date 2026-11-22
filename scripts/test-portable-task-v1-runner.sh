#!/usr/bin/env bash
# Self-contained rejection probe: never starts a compiler or copies private source.
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
out="$(mktemp)"
trap 'rm -f -- "$out"' EXIT
if (unset ATTO_TASK_SUITE_ROOT ATTO_TASK_SUITE_SHA; bash "$root/scripts/run-portable-task-v1-guest.sh") >"$out" 2>&1; then
 echo 'FAIL: unconfigured Task runner was accepted' >&2
 exit 1
fi
grep -q 'ATTO_TASK_SUITE_ROOT' "$out" || { cat "$out" >&2; exit 1; }
echo 'PASS: portable Task guest runner refuses missing authorized suite'
