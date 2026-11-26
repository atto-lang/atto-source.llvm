#!/usr/bin/env bash
set -euo pipefail
compiler="$1"
clang="$2"
fixture="$3"
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
runtime="$root/src/main/cpp/oreslang/runtime/NativeStringRegion.c"
test -f "$runtime"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
# Use the real native string runtime; never replace guest output with stubs.
"$compiler" "$fixture" > "$work/guest.ll"
grep -F 'call void @cleanup' "$work/guest.ll" >/dev/null
for opt in 0 2; do
    "$clang" "-O$opt" -x ir "$work/guest.ll" -x c "$runtime" -std=c11 -pthread -I "$root/src/main/cpp" -o "$work/guest-O$opt"
    # The runtime deliberately rejects regular-file stdout. Give the guest
    # a FIFO and capture downstream without changing native NIO policy.
    set +e
    "$work/guest-O$opt" | cat > "$work/stdout-O$opt"
    statuses=("${PIPESTATUS[@]}")
    set -e
    printf 'guest O%s exit=%s collector=%s\n' "$opt" "${statuses[0]}" "${statuses[1]}"
    test "${statuses[0]}" = 42
test "${statuses[1]}" = 0
    printf '1\n2\n7\n10\n' > "$work/expected"
    cmp "$work/expected" "$work/stdout-O$opt"
    printf 'NATIVE_DEFER_PIPELINE_PASS O%s\n' "$opt"
    sha256sum "$work/guest.ll" "$work/guest-O$opt"
done
"$clang" -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -x ir "$work/guest.ll" -x c "$runtime" -std=c11 -pthread -I "$root/src/main/cpp" -o "$work/guest-sanitized"
set +e
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 "$work/guest-sanitized" | cat > "$work/stdout-sanitized"
statuses=("${PIPESTATUS[@]}")
set -e
printf 'guest ASAN_UBSAN exit=%s collector=%s\n' "${statuses[0]}" "${statuses[1]}"
test "${statuses[0]}" = 42
test "${statuses[1]}" = 0
cmp "$work/expected" "$work/stdout-sanitized"
printf 'NATIVE_DEFER_PIPELINE_PASS ASAN_UBSAN\n'
