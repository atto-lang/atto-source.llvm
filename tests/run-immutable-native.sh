#!/usr/bin/env bash
set -euo pipefail
if [[ "$#" -ne 4 ]]; then echo 'usage: run-immutable-native.sh rustc compiler clang fixtures' >&2; exit 2; fi
rustc_bin="$1"; compiler="$2"; clang="$3"; fixtures="$4"
dir="$(mktemp -d)"
trap 'rm -rf -- "$dir"' EXIT
"$rustc_bin" --edition=2021 -O -- "${BASH_SOURCE[0]%/*}/immutable_default_native.rs" -o "$dir/immutable-default-runner"
"$dir/immutable-default-runner" "$compiler" "$clang" "$fixtures"
