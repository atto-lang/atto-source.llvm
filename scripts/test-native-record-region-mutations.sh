#!/usr/bin/env bash
# Native host ABI and two fail-closed mutations; not compiler-backed guest proof.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
workspace="$(mktemp -d)"
trap 'rm -rf "$workspace"' EXIT
ulimit -c 0
compiler="${CC:-cc}"
source="$root/src/main/cpp/oreslang/runtime/NativeRecordRegion.c"
test_source="$root/tests/native_record_region_test.c"
includes=("-I$root/src/main/cpp" "-I$root/src/main/cpp/oreslang/runtime")
flags=(-std=c11 -O1 -g -Wall -Wextra -Werror -pthread -DORESLANG_RECORD_TEST_ALLOC_FAILURE=1)
"$compiler" "${flags[@]}" "${includes[@]}" "$test_source" "$source" -o "$workspace/baseline"
"$workspace/baseline"

# Exactly one match per source mutation, otherwise fail before running tests.
replace_exact_once() {
    local original="$1" replacement="$2" output="$3"
    awk -v needle="$original" -v new="$replacement" '
        {
            at = index($0, needle)
            if (at) {
                replacements++
                $0 = substr($0, 1, at - 1) new substr($0, at + length(needle))
            }
            print
        }
        END { if (replacements != 1) exit 9 }
    ' "$source" > "$output"
}
check_killed_mutant() {
    local label="$1" file="$2" output="$workspace/$1"
    "$compiler" "${flags[@]}" "${includes[@]}" "$test_source" "$file" -o "$output"
    local status=0
    "$output" > "$workspace/${label}.log" 2>&1 || status=$?
    if (( status == 0 )); then
        echo "FAIL: mutation $label survived" >&2
        return 1
    fi
    if ! grep -q '^record region: line ' "$workspace/${label}.log"; then
        echo "FAIL: $label exited without a test assertion; not mutation evidence" >&2
        cat "$workspace/${label}.log" >&2
        return 1
    fi
    printf 'PASS: mutation %s killed by native assertion (exit %s)\n' "$label" "$status"
}
replace_exact_once '!(record->writable_fields & (UINT64_C(1) << (unsigned)field))' '0' "$workspace/no-write-mask.c"
check_killed_mutant writable-mask "$workspace/no-write-mask.c"
replace_exact_once '(uint32_t)(token >> 32) != r->generation' '0' "$workspace/no-generation-fence.c"
check_killed_mutant generation-fence "$workspace/no-generation-fence.c"
