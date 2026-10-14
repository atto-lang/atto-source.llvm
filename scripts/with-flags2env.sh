#!/usr/bin/env bash
# Primary entrypoint for standalone compiler/runtime binaries.
# Usage: bash scripts/with-flags2env.sh /path/to/oreslang-binary [oreslang-options] source.ores
# JVM hosts with Java launcher flags should parse only the guest argv before
# invoking the JVM; do not feed -cp/-p/-m to flags2env as guest options.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
if (($# < 1)); then
  echo "usage: with-flags2env.sh <oreslang-binary> [guest-cli-args...]" >&2
  exit 2
fi
backend="$1"
shift
f2e="${FLAGS2ENV_BIN:-flags2env}"
if ! command -v "$f2e" >/dev/null 2>&1; then
  echo "flags2env binary not found (install github.com/flags-2-env/flags-2-env)" >&2
  exit 2
fi
# Parse through upstream structured parser: never silently fall back on
# config/argument errors. The upstream CLI shell-quotes every export.
exports="$("$f2e" shell-env --config "$root/.cli-flags.toml" -- "$backend" "$@")"
eval "$exports"
exec "$backend" "$@"
