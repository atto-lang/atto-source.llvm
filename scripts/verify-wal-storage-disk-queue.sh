#!/bin/sh
# Pinned native host disk worker test, NOT Oreslang compiler/guest execution.
set -eu
export LC_ALL=C
sh scripts/verify-wal-storage-fd-owner.sh
command -v timeout >/dev/null 2>&1
scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT HUP INT TERM
"$DB_CXX_BIN" -std=c++17 -Wall -Wextra -Werror -pthread -I src/main/cpp \
  tests/native/wal_storage_disk_queue_test.cpp -o "$scratch/wal-disk-test"
timeout 30s "$scratch/wal-disk-test" >"$scratch/out" 2>"$scratch/err" || {
  cat "$scratch/err" >&2; exit 1;
}
printf '%s\n' 'WAL_DISK_QUEUE_HOST_PASS checks=25; NO ACTOR PIN OR GUEST PROOF' \
  >"$scratch/expected"
cmp -s "$scratch/out" "$scratch/expected" || { cat "$scratch/out" >&2; exit 1; }
printf 'NATIVE_OFF_CARRIER_HOST_CHECKS=25 NO_ACTOR_PIN NO_GUEST_PROOF\n'
