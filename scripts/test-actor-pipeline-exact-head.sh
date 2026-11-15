#!/usr/bin/env bash
# Exact-source LLVM C++ actor host pipeline gate. Does not compile .atto guests.
set -euo pipefail
cd "$(dirname "$0")/.."
command -v git >/dev/null || exit 127
command -v g++ >/dev/null || {
  echo "BLOCKED_NO_GCC: LLVM host pipeline tests not executed" >&2
  exit 127
}
command -v clang++ >/dev/null || {
  echo "BLOCKED_NO_CLANG: LLVM host pipeline tests not executed" >&2
  exit 127
}
head="$(git rev-parse HEAD)"
tree="$(git rev-parse 'HEAD^{tree}')"
if [ -n "${EXPECTED_HEAD:-}" ] && [ "$head" != "$EXPECTED_HEAD" ]; then
  echo "FAILED_SOURCE_HEAD: expected=$EXPECTED_HEAD actual=$head" >&2
  exit 1
fi
if [ -n "$(git status --porcelain)" ]; then
  echo "FAILED_DIRTY_SOURCE: host actor pipeline matrix requires clean checkout" >&2
  exit 1
fi
printf 'LLVM_ACTOR_PIPELINE_SOURCE_HEAD=%s\n' "$head"
printf 'LLVM_ACTOR_PIPELINE_SOURCE_TREE=%s\n' "$tree"
g++ --version | head -n 1
clang++ --version | head -n 1
log="${ACTOR_PIPELINE_LOG:-${RUNNER_TEMP:-${TMPDIR:-/tmp}}/actor-pipeline-host.log}"
mkdir -p "$(dirname "$log")"
sh scripts/test-actor-pipeline-routing.sh | tee "$log"
# Require both result markers; no false pass on a zero-step/syntax-only run.
grep -Fx 'ORES_ACTOR_PIPELINE_HOST_MATRIX_PASS' "$log"
grep -Fx 'ORES_ACTOR_IO_CONTROL_HOST_MATRIX_PASS' "$log"
echo 'LLVM_ACTOR_PIPELINE_EXACT_HEAD_HOST_EXECUTED_PASS'
