#!/usr/bin/env bash
# HOST-ONLY negative mutation: deleting either STOP gate must make the
# real actor scheduler regression fail on the expected revoked-read assertion.
# Builds private repository source only in this checkout's temporary directory.
set -euo pipefail
ulimit -c 0 || true
cd "$(git rev-parse --show-toplevel)"
CXX="${CXX:-g++}"
case "$CXX" in
  g++) CC=gcc ;;
  clang++) CC=clang ;;
  *) echo "Unsupported compiler: $CXX" >&2; exit 2 ;;
esac
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
"$CC" -std=c11 -O2 -UNDEBUG -Wall -Wextra -Wpedantic -Werror -pthread \
  -Isrc/main/cpp -c src/main/cpp/oreslang/runtime/NativeActorScheduler.c \
  -o "$work/scheduler.o"
for mutation in postcopy stopgate; do
  mkdir -p "$work/include/oreslang/runtime"
  python3 - "src/main/cpp/oreslang/runtime/ActorLocalSchedulerTurnV1.hpp" \
              "$work/include/oreslang/runtime/ActorLocalSchedulerTurnV1.hpp" \
              "$mutation" <<'PY'
from pathlib import Path
import sys
source, dest, mutation = sys.argv[1:]
original = Path(source).read_text()
changes = {
    'postcopy': (
        "    if (!snapshot || !actor_local_current_turn_v1(scheduler, actor, stamp))\n"
        "        return std::nullopt;\n",
        "    if (!snapshot) return std::nullopt;\n"),
    'stopgate': (
        "        ores_native_actor_scheduler_poll_ready(scheduler, actor);",
        "        true;"),
}
needle, replacement = changes[mutation]
if original.count(needle) != 1:
    raise SystemExit('MUTATION_ANCHOR_INVALID: '+mutation)
Path(dest).write_text(original.replace(needle, replacement, 1))
PY
  "$CXX" -std=c++17 -O2 -UNDEBUG -Wall -Wextra -Wpedantic -Werror -pthread \
    -I"$work/include" -Isrc/main/cpp \
    tests/native_actorlocal_stop_during_copy_v1_test.cpp \
    "$work/scheduler.o" -o "$work/mutant-$mutation"
  status=0
  ("$work/mutant-$mutation") >"$work/$mutation.log" 2>&1 || status=$?
  if [[ "$status" -ne 134 ]]; then
    echo "MUTATION_INVALID_EXIT mutation=$mutation exit=$status expected=134" >&2
    cat "$work/$mutation.log" >&2
    exit 1
  fi
  if ! grep -F 'native_actorlocal_stop_during_copy_v1_test.cpp:' "$work/$mutation.log" \
      | grep -F 'Assertion' | grep -Fq '!snapshot'; then
    echo "MUTATION_INVALID_ASSERTION mutation=$mutation" >&2
    cat "$work/$mutation.log" >&2
    exit 1
  fi
  printf 'ACTORLOCAL_STOP_MUTATION_CAUGHT mutation=%s compiler=%s\n' "$mutation" "$CXX"
done
