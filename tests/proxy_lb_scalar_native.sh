#!/usr/bin/env bash
# Native compiler guest proof; linux-only; exact pinned source blobs.
# Not permission to run untrusted PR code on privileged self-hosted runners.
set -euo pipefail
[[ $# == 3 ]] || { echo "usage: bash proxy_lb_scalar_native.sh LLVM_COMPILER CLANG CXX" >&2; exit 64; }
compiler="$1"; clang="$2"; cxx="$3"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
tmp="$(mktemp -d)"
trap 'rm -rf -- "$tmp"' EXIT
"$cxx" -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror \
  "$root/tests/proxy_lb_scalar_guest_probe.cpp" -ldl -o "$tmp/probe"
names=(comparison-positive comparison-negative comparison-tie comparison-max-balanced comparison-max-positive)
hashes=(885b62c32ea5f25a7223afd166a78ba60642dde7 \
        9e9a6c048367dc52dda7948072920717d3e8d357 \
        b100715d0a8544e13ad8d88bf44a7fced72f24dd \
        1195e932ce4e128b0d109bf3715afbc82d85b567 \
        23a9c6371e252ff4e1ffbeed87f05238e9268296)
values=(6 -1 0 0 16384)
for i in "${!names[@]}"; do
  name="${names[$i]}"
  source="$root/tests/fixtures/proxy-lb-v1/$name.ores"
  actual="$(git hash-object -- "$source")"
  [[ "$actual" == "${hashes[$i]}" ]] || {
    echo "LB scalar source hash changed: $name" >&2; exit 65;
  }
  "$compiler" "$source" > "$tmp/$name.ll"
  test -s "$tmp/$name.ll"
  "$clang" -O1 -fPIC -shared -x ir "$tmp/$name.ll" -o "$tmp/$name.so"
  "$tmp/probe" "$tmp/$name.so" "${values[$i]}"
done
printf 'LLVM_PROXY_LB_FIVE_GUEST_FIXTURES_PASS source_only=NO\n'
