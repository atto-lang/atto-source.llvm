#!/usr/bin/env bash
# Run against the actual built native compiler; no simulation.
set -euo pipefail
compiler="${1:?usage: $0 /absolute/path/to/oreslang-llvmc}"
[[ -x "$compiler" ]] || { echo "WITH_LLVM_COMPILER_NOT_EXECUTED" >&2; exit 2; }
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
for name in with-scalar-reject with-trivia-reject; do
    if "$compiler" "$root/tests/with-ownership-v1/$name.atto" >"$tmp/$name.ll" 2>"$tmp/$name.err"; then
        echo "WITH_UNSOUND_NATIVE_ADMISSION: $name" >&2; exit 1
    fi
    grep -q 'WITH_BACKEND_UNSUPPORTED:' "$tmp/$name.err"
    test ! -s "$tmp/$name.ll"
done
for name in with-inert-comment with-substring-identifier with-call-identifier; do
    "$compiler" "$root/tests/with-ownership-v1/$name.atto" >"$tmp/$name.ll" 2>"$tmp/$name.err"
    grep -q 'define ' "$tmp/$name.ll"
done
echo WITH_LLVM_COMPILER_FRONTEND_GATE_PASS
