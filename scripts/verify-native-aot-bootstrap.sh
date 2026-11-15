#!/usr/bin/env bash
set -Eeuo pipefail

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"
compiler="${1:-build/oreslang-llvmc}"
clang_bin="${2:-$(command -v clang || true)}"
[[ -x "$compiler" ]] || { echo "missing oreslang-llvmc: $compiler" >&2; exit 2; }
[[ -n "$clang_bin" && -x "$clang_bin" ]] || { echo "clang is required" >&2; exit 2; }

work="$(mktemp -d "${TMPDIR:-/tmp}/ores-llvm-bootstrap.XXXXXXXX")"
trap 'rm -rf -- "$work"' EXIT
ir="$work/bootstrap.ll"
obj="$work/bootstrap.o"
exe="$work/bootstrap"

"$compiler" tests/fixtures/constant-main.ores >"$ir" 2>"$work/compiler.stderr"
test ! -s "$work/compiler.stderr" || { cat "$work/compiler.stderr" >&2; exit 1; }
grep -q '^; ModuleID' "$ir" || { echo "compiler did not emit LLVM IR" >&2; exit 1; }
grep -q '^define .*@main(' "$ir" || { echo "compiler IR has no native main" >&2; exit 1; }

"$clang_bin" -O2 -c -x ir "$ir" -o "$obj"
test -s "$obj" || { echo "native object was not emitted" >&2; exit 1; }
file "$obj" | grep -Eq 'ELF .* relocatable|ELF .* object' || {
  echo "bootstrap object is not ELF" >&2; file "$obj" >&2; exit 1;
}

"$clang_bin" -O2 "$obj" -o "$exe"
test -x "$exe" || { echo "native executable missing" >&2; exit 1; }
file "$exe" | grep -q 'ELF 64-bit' || {
  echo "bootstrap executable is not ELF64" >&2; file "$exe" >&2; exit 1;
}

set +e
"$exe"
status=$?
set -e
if [[ "$status" -ne 42 ]]; then
  echo "native bootstrap fixture exit=$status expected=42" >&2
  exit 1
fi

printf 'BOOTSTRAP_NATIVE_AOT_OK compiler=%s object_sha256=%s exe_sha256=%s\n'   "$(git rev-parse --verify HEAD)"   "$(sha256sum "$obj" | awk '{print $1}')"   "$(sha256sum "$exe" | awk '{print $1}')"
