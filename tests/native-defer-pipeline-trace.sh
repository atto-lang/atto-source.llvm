#!/usr/bin/env bash
set -euo pipefail
compiler="$1"
clang="$2"
fixture="$3"
work=$(mktemp -d)
# Keep the temporary receipt available on failure; never mutate source.
"$compiler" "$fixture" > "$work/guest.ll"
grep -F 'call void @cleanup' "$work/guest.ll" >/dev/null
for opt in 0 2; do
    "$clang" "-O$opt" -x ir "$work/guest.ll" -o "$work/guest-O$opt"
    status=0
    "$work/guest-O$opt" > "$work/stdout-O$opt" || status=$?
    test "$status" = 42
    printf '1\n2\n7\n10\n' > "$work/expected"
    cmp "$work/expected" "$work/stdout-O$opt"
    printf 'NATIVE_DEFER_PIPELINE_PASS O%s\n' "$opt"
    sha256sum "$work/guest.ll" "$work/guest-O$opt"
done
