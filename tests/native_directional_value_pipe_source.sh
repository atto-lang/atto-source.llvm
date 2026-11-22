#!/usr/bin/env bash
# Real Atto .atto -> oreslang-llvmc LLVM IR -> Clang -> native executable.
# Every fixture is byte-identical to the Java and .NET directional corpus.
set -euo pipefail
compiler=${1:?native compiler}
clang=${2:?clang}
root=${3:?source root}
fixtures="$root/tests/fixtures/directional-parity"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

for entry in directional-value-forward-v1 directional-value-reverse-v1 directional-rpipe-contextual-name; do
    for opt in 0 2; do
        "$compiler" --target=host "$fixtures/$entry.atto" > "$tmp/$entry.ll" 2> "$tmp/$entry.compile.err"
        test -s "$tmp/$entry.ll"
        "$clang" "-O$opt" -x ir "$tmp/$entry.ll" -o "$tmp/$entry-O$opt"
        set +e
        "$tmp/$entry-O$opt" > "$tmp/$entry.stdout" 2> "$tmp/$entry.stderr"
        status=$?
        set -e
        if [[ "$status" -ne 42 ]]; then
            echo "native pipeline guest $entry at O$opt exited $status instead of 42" >&2
            cat "$tmp/$entry.compile.err" "$tmp/$entry.stderr" >&2
            exit 1
        fi
    done
done

for case_name in directional-value-mixed-reject directional-actor-rpipe-reject; do
    diagnostic=E_VALUE_PIPE_DIRECTION
    if [[ "$case_name" == directional-actor-rpipe-reject ]]; then
        diagnostic=E_ACTOR_PIPE_BACKEND_UNSUPPORTED
    fi
    if "$compiler" --target=host "$fixtures/$case_name.atto" >"$tmp/negative.ll" 2>"$tmp/negative.err"; then
        echo "unsafe directional pipeline $case_name compiled unexpectedly" >&2
        exit 1
    fi
    if [[ -s "$tmp/negative.ll" ]] || ! grep -Fq "$diagnostic" "$tmp/negative.err"; then
        echo "negative case $case_name emitted IR or wrong diagnostic" >&2
        cat "$tmp/negative.err" >&2
        exit 1
    fi
done
echo "NATIVE_DIRECTIONAL_PIPELINE_PASS: forward=42 reverse=42 contextual=42 O0/O2 negatives=2"
