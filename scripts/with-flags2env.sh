#!/usr/bin/env bash
# Canonical flags2env launch boundary for standalone Oreslang binaries.
# Use the upstream CLI's checked .cli-flags.toml and never eval shell output.
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
  echo "flags2env binary not found; install github.com/flags-2-env/flags-2-env" >&2
  exit 2
fi
if ! command -v python3 >/dev/null 2>&1; then
  echo "python3 required for the no-eval flags2env launch boundary" >&2
  exit 2
fi

# Never source/eval the shell export stream. Treat it strictly as data and
# permit only schema-declared Oreslang option keys. Keep cwd unchanged when
# launching the compiler so relative source paths preserve their meaning.
exec python3 - "$f2e" "$root/.cli-flags.toml" "$backend" "$@" <<'PY'
import os
import re
import shlex
import subprocess
import sys

f2e, config, binary, *args = sys.argv[1:]
allowed = {
    "ORES_CORE_PERF", "ORES_CORE_DEBUG", "ORES_STRICT_ISOLATE",
    "ORES_CHECK", "ORES_BUILD_ANALYSIS", "ORES_MODE", "ORES_PLATFORM",
    "ORES_PERMISSION_CHECK", "ORES_FLAGS2ENV_ERRORS",
    "ORES_LLVM_TARGET", "ORES_LLVM_CPU",
    "ORES_LLVM_EXPERIMENTAL_OWNED_INT",
    "ORES_LLVM_EXPERIMENTAL_RT_SCALAR_LOOP",
    "ORES_LLVM_UNKNOWN_OPTIONS", "ORES_LLVM_FLAGS2ENV_ERRORS",
}
try:
    result = subprocess.run(
        [f2e, "shell-env", "--config", config, "--", binary, *args],
        capture_output=True, text=True, timeout=30, check=False,
    )
except (OSError, subprocess.SubprocessError) as error:
    print(f"flags2env invocation failed: {error}", file=sys.stderr)
    sys.exit(2)
if result.returncode:
    sys.stderr.write(result.stderr)
    sys.exit(result.returncode if 1 <= result.returncode <= 125 else 2)
if result.stderr:
    print("flags2env produced unexpected diagnostic output", file=sys.stderr)
    sys.exit(2)
if len(result.stdout) > 1024 * 1024:
    print("flags2env output exceeds 1 MiB", file=sys.stderr)
    sys.exit(2)

env = os.environ.copy()
seen = set()
for line in result.stdout.splitlines():
    if not line.startswith("export ") or "=" not in line:
        print("flags2env emitted non-export content", file=sys.stderr)
        sys.exit(2)
    key, raw = line[7:].split("=", 1)
    if not re.fullmatch(r"[A-Z][A-Z0-9_]*", key) or key not in allowed or key in seen:
        print("flags2env emitted an unauthorized or duplicate variable", file=sys.stderr)
        sys.exit(2)
    seen.add(key)
    try:
        tokens = shlex.split(raw, posix=True)
    except ValueError:
        print("flags2env emitted malformed quoting", file=sys.stderr)
        sys.exit(2)
    if len(tokens) != 1 or "\x00" in tokens[0] or "\n" in tokens[0]:
        print("flags2env emitted an invalid value", file=sys.stderr)
        sys.exit(2)
    env[key] = tokens[0]

try:
    os.execvpe(binary, [binary, *args], env)
except OSError as error:
    print(f"unable to execute Oreslang binary: {error}", file=sys.stderr)
    sys.exit(126)
PY
