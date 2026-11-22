#!/bin/sh
# HOST MODEL ONLY: this does not execute guest .atto source or link std/env.
set -eu
cd "$(dirname "$0")/.."
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
cc_bin="${CC:-cc}"
"$cc_bin" -std=c11 -O2 -Wall -Wextra -Werror -pedantic \
    -I native/include \
    native/src/host_env_snapshot_v1.c \
    native/tests/host_env_snapshot_v1_test.c \
    -o "$tmp/host-env-v1"
"$tmp/host-env-v1"
printf 'HOST_ONLY_PASS: std/env scoped native snapshot (not guest parity)\n'
