#!/usr/bin/env bash
# Compile and execute native actor CONTROL contracts without LLVM/Clang CMake.
# This is HOST-ONLY evidence, not proof of guest Error lowering or AOT execution.
set -euo pipefail

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"

# Print the actual checkout identity alongside fixture output. CI can set
# ORESLANG_EXPECTED_SOURCE_SHA to a full immutable source commit for a strict
# exact-head gate. This detects accidental branch drift, not authorization
# for copying source into another organization.
checkout_sha=""
if command -v git >/dev/null 2>&1; then
    checkout_sha="$(git -C "$root" rev-parse --verify 'HEAD^{commit}' 2>/dev/null || true)"
fi
printf 'Oreslang fixture checkout SHA: %s\n' "${checkout_sha:-unavailable}" >&2
if [[ -n "${ORESLANG_EXPECTED_SOURCE_SHA:-}" ]]; then
    if [[ ! "${ORESLANG_EXPECTED_SOURCE_SHA}" =~ ^[0-9a-f]{40}$ ]] \
            || [[ "$checkout_sha" != "${ORESLANG_EXPECTED_SOURCE_SHA}" ]]; then
        printf 'Exact-head verification refused: expected %s, actual %s\n' \
            "$ORESLANG_EXPECTED_SOURCE_SHA" "${checkout_sha:-unavailable}" >&2
        exit 2
    fi
    # A Git HEAD from a parent (or unrelated) worktree cannot prove this
    # script's source tree. Ignored nested directories may be completely
    # absent from that parent's tracked Git objects while status is clean.
    checkout_top=""
    if ! checkout_top="$(git -C "$root" rev-parse --show-toplevel 2>/dev/null)" \
            || [[ "$(cd -- "$checkout_top" && pwd -P)" != "$root" ]]; then
        printf 'Exact-head verification refused: script directory is not the Git worktree root\n' >&2
        exit 2
    fi
    # A matching HEAD hash is not a source-integrity proof when tracked files
    # or untracked compiler inputs were changed after checkout. Verify the
    # entire worktree before invoking any compiler or test process.
    checkout_status=""
    if ! checkout_status="$(git -C "$root" status --porcelain --untracked-files=all 2>/dev/null)"; then
        printf 'Exact-head verification refused: unable to inspect checkout cleanliness\n' >&2
        exit 2
    fi
    if [[ -n "$checkout_status" ]]; then
        printf 'Exact-head verification refused: checkout contains modified or untracked files\n' >&2
        exit 2
    fi
fi

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
