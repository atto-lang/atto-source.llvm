#!/usr/bin/env bash
# Tests exact repository header + real Parser.hpp AST types; NO LLVM required.
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"
compiler="${1:-c++}"
command -v "$compiler" >/dev/null || { echo "missing compiler: $compiler" >&2; exit 2; }
tmp="$(mktemp -d)"
trap 'rm -rf -- "$tmp"' EXIT
common=(-std=c++17 -O1 -g -Wall -Wextra -Wpedantic -Werror
        -UNDEBUG -fsanitize=address,undefined -fno-omit-frame-pointer
        -I src/main/cpp)
for fixture in ebpf_xdp_admission_test ebpf_xdp_action_proof_property_test; do
    "$compiler" "${common[@]}" "tests/${fixture}.cpp" -o "$tmp/$fixture"
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
    UBSAN_OPTIONS=halt_on_error=1 "$tmp/$fixture"
done
echo "PASS exact source header XDP admission + randomized action property ($compiler)"
