#!/usr/bin/env bash
# Compile and execute native actor CONTROL contracts without LLVM/Clang CMake.
# This is HOST-ONLY evidence, not proof of guest Error lowering or AOT execution.
set -euo pipefail

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
compiler="${CXX:-c++}"
if ! command -v "$compiler" >/dev/null 2>&1; then
    printf 'No C++17 compiler found: %s\n' "$compiler" >&2
    exit 2
fi

work="$(mktemp -d "${TMPDIR:-/tmp}/oreslang-actor-control.XXXXXXXX")"
trap 'rm -rf -- "$work"' EXIT

for fixture in \
    actor_error_catalog_test \
    actor_error_effect_policy_test \
    actor_failure_control_concurrency_test \
    actor_local_failure_policy_test \
    supervisor_failure_lane_test \
    supervisor_control_turn_test \
    effect_supervision_bridge_test \
    effect_supervisor_end_to_end_test
do
    printf 'Building host-only %s\n' "$fixture"
    "$compiler" -std=c++17 -Wall -Wextra -Wpedantic -Werror \
        -pthread -I "$root/src/main/cpp" \
        "$root/tests/$fixture.cpp" -o "$work/$fixture"
    printf 'Running host-only %s\n' "$fixture"
    "$work/$fixture"
done

printf 'PASS: 8 native host CONTROL fixtures (not guest/compiler parity).\n'
