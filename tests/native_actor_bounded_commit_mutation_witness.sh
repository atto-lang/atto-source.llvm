#!/usr/bin/env bash
# Host-only negative mutation. Do not accept "any crash" as a safety proof.
# Mutated files are temporary and never overwrite the working checkout.
set -euo pipefail
ulimit -c 0 || true
cd "$(git rev-parse --show-toplevel)"
CC="${CC:-gcc}"
case "$CC" in gcc|clang) ;; *) echo "Unsupported C compiler: $CC" >&2; exit 2 ;; esac
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
for mutation in foreign-carrier stop-race; do
  python3 - "src/main/cpp/oreslang/runtime/NativeActorScheduler.c" \
    "$work/mutated-scheduler.c" "$mutation" <<'PY'
from pathlib import Path
import sys
src, dest, mode = sys.argv[1:]
whole = Path(src).read_text()
start = whole.index('int ores_native_actor_scheduler_commit_if_running_v1(')
end = whole.index('/* Actor-specific authority', start)
fn = whole[start:end]
precheck = """    if (!s || !commit || executing != s || !executing_claim.carrier
            || !actor.scheduler_domain
            || actor.scheduler_domain != executing_actor.scheduler_domain
            || actor.actor_slot != executing_actor.actor_slot
            || actor.generation != executing_actor.generation)
        return 0;"""
owner = """        && ores_execution_owns_running(
            &s->actors[actor.actor_slot].authority, executing_claim);"""
if fn.count(precheck) != 1 or fn.count(owner) != 1:
    raise SystemExit('MUTATION_ANCHOR_INVALID: '+mode)
if mode == 'foreign-carrier':
    fn = fn.replace(precheck, '    if (!s || !commit) return 0;', 1)
    fn = fn.replace(owner,
        '        && ores_execution_phase(&s->actors[actor.actor_slot].authority) == ORES_EXEC_RUNNING;', 1)
elif mode == 'stop-race':
    retired = '        && !s->actors[actor.actor_slot].retired'
    if fn.count(retired) != 1:
        raise SystemExit('MUTATION_RETIRED_FENCE_MISSING')
    fn = fn.replace(retired, '', 1)
    fn = fn.replace(owner,
        '        && ores_execution_owns(\n            &s->actors[actor.actor_slot].authority, executing_claim);', 1)
else:
    raise SystemExit('UNKNOWN_MUTATION')
Path(dest).write_text(whole[:start]+fn+whole[end:])
PY
  "$CC" -std=c11 -O2 -UNDEBUG -Wall -Wextra -Wpedantic -Werror -pthread \
    -Isrc/main/cpp -c "$work/mutated-scheduler.c" -o "$work/scheduler.o"
  "$CC" -std=c11 -O2 -UNDEBUG -Wall -Wextra -Wpedantic -Werror -pthread \
    -Isrc/main/cpp tests/native_actor_bounded_commit_carrier_stop_v1_test.c \
    "$work/scheduler.o" -o "$work/mutant"
  status=0
  ("$work/mutant") >"$work/$mutation.log" 2>&1 || status=$?
  if [[ "$status" -ne 134 ]]; then
    echo "BOUNDED_COMMIT_MUTATION_INVALID_EXIT mode=$mutation status=$status" >&2
    cat "$work/$mutation.log" >&2
    exit 1
  fi
  if ! grep -F 'native_actor_bounded_commit_carrier_stop_v1_test.c:' "$work/$mutation.log" \
      | grep -F 'Assertion' \
      | grep -Fq '!ores_native_actor_scheduler_commit_if_running_v1'; then
    echo "BOUNDED_COMMIT_MUTATION_UNRELATED_FAILURE mode=$mutation" >&2
    cat "$work/$mutation.log" >&2
    exit 1
  fi
  printf 'BOUNDED_COMMIT_MUTATION_CAUGHT mode=%s compiler=%s\n' "$mutation" "$CC"
done
