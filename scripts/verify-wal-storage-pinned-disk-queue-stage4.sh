#!/bin/sh
# Native production-header actor-lifetime loan test. NEVER substitute a shim.
set -eu
export LC_ALL=C
sh scripts/verify-wal-storage-disk-queue.sh
: "${DB_CXX_BIN:?pin a native compiler with DB_CXX_BIN}"
command -v timeout >/dev/null 2>&1
scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT HUP INT TERM
"$DB_CXX_BIN" -std=c++17 -Wall -Wextra -Werror -pthread -I src/main/cpp \
    tests/native/wal_storage_pinned_disk_queue_test.cpp -o "$scratch/wal-native-pinned"
timeout 30s "$scratch/wal-native-pinned" >"$scratch/out" 2>"$scratch/err" || {
    cat "$scratch/err" >&2
    exit 1
}
printf '%s\n' 'WAL_NATIVE_PINNED_QUEUE_PASS checks=14; NO GUEST OR POWER-LOSS PROOF' \
    >"$scratch/expected"
cmp -s "$scratch/out" "$scratch/expected" || {
    cat "$scratch/out" >&2
    exit 1
}
printf 'NATIVE_ACTOR_GROUP_PRODUCTION_HEADER_CHECKS=14 NO_GUEST_EXECUTION\n'
