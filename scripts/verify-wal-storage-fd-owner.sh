#!/bin/sh
# Exact-head LLVM POSIX host primitive verification, not compiler-backed .ores.
set -eu
export LC_ALL=C
: "${DB_EXPECTED_COMMIT:?independent source commit required}"
: "${DB_EXPECTED_TREE:?independent source tree required}"
: "${DB_CXX_BIN:?absolute C++17 compiler required}"
: "${DB_CXX_SHA256:?independent compiler binary SHA-256 required}"
case "$DB_CXX_BIN" in /*) ;; *) echo 'compiler path must be absolute' >&2; exit 2;; esac
for hash in "$DB_CXX_SHA256"; do
  case "$hash" in *[!0123456789abcdef]*|'') echo 'bad compiler SHA-256' >&2; exit 2;; esac
  test "${#hash}" -eq 64 || exit 2
done
for hash in "$DB_EXPECTED_COMMIT" "$DB_EXPECTED_TREE"; do
  case "$hash" in *[!0123456789abcdef]*|'') echo 'bad source pin' >&2; exit 2;; esac
  test "${#hash}" -eq 40 || exit 2
done
test "$(git rev-parse --show-toplevel)" = "$PWD"
test -z "$(git status --porcelain)"
test "$(git rev-parse HEAD)" = "$DB_EXPECTED_COMMIT"
test "$(git rev-parse HEAD^{tree})" = "$DB_EXPECTED_TREE"
printf '%s  %s\n' "$DB_CXX_SHA256" "$DB_CXX_BIN" | sha256sum -c - >/dev/null
command -v timeout >/dev/null 2>&1
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
"$DB_CXX_BIN" -std=c++17 -Wall -Wextra -Werror -pthread -I src/main/cpp \
  tests/native/wal_storage_fd_owner_test.cpp -o "$tmp/test"
timeout 30s "$tmp/test" >"$tmp/out" 2>"$tmp/err" || { cat "$tmp/err" >&2; exit 1; }
printf '%s\n' 'WAL_NATIVE_HOST_FD_PASS checks=26; NO GUEST OR POWER-LOSS CLAIM' >"$tmp/expected"
cmp -s "$tmp/out" "$tmp/expected" || { cat "$tmp/out" >&2; exit 1; }
printf 'SOURCE_COMMIT=%s SOURCE_TREE=%s NATIVE_HOST_CHECKS=26 NO_GUEST_PROOF\n' \
  "$DB_EXPECTED_COMMIT" "$DB_EXPECTED_TREE"
