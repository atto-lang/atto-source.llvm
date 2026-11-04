#!/usr/bin/env bash
# Real guest compiler -> LLVM IR -> Clang native executable -> exit-code oracle.
# No interpreted substitute and no success when either toolchain is missing.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
source="$root/examples/dop-cache/soa-int-update.atto"
compiler="${ORESLANG_LLVMC:-$root/build/oreslang-llvmc}"
clang="${CLANG:-clang}"
[[ -f "$source" && ! -L "$source" && "${source##*.}" == "atto" ]] || {
  echo "missing native .atto guest fixture" >&2; exit 66;
}
[[ -x "$compiler" && -f "$compiler" ]] || {
  echo "NOT_EXECUTED: pinned LLVM guest compiler is unavailable" >&2; exit 69;
}
compiler="$(cd "$(dirname "$compiler")" && pwd -P)/$(basename "$compiler")"
clang_path="$(command -v "$clang")" || {
  echo "NOT_EXECUTED: Clang native linker is unavailable" >&2; exit 69;
}
clang_path="$(cd "$(dirname "$clang_path")" && pwd -P)/$(basename "$clang_path")"
[[ -x "$clang_path" && -f "$clang_path" ]] || {
  echo "NOT_EXECUTED: Clang native linker is not a regular executable" >&2; exit 69;
}
compiler_before="$(sha256sum "$compiler")"
clang_before="$(sha256sum "$clang_path")"
source_before="$(sha256sum "$source")"
# A hanging or runaway guest is never a successful AOT smoke result.
command -v timeout >/dev/null 2>&1 || {
  echo "NOT_EXECUTED: coreutils timeout is unavailable" >&2; exit 69;
}
work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT
# Compiler and linker can also stall. Bound the whole source-to-native
# pipeline; a timeout (124/137) or nonzero compile is never a guest pass.
if timeout --signal=TERM --kill-after=3s 180s \
    env ORESLANG_LLVMC="$compiler" CLANG="$clang_path" \
    bash "$root/scripts/compile-aot.sh" "$source" "$work/guest"; then
  :
else
  status=$?
  echo "FAIL: LLVM guest compilation/link failed or timed out (status $status)" >&2
  exit 1
fi
[[ "$(sha256sum "$compiler")" == "$compiler_before" ]] || {
  echo "LLVM guest compiler changed during AOT verification" >&2; exit 1;
}
[[ "$(sha256sum "$clang_path")" == "$clang_before" ]] || {
  echo "Clang changed during AOT verification" >&2; exit 1;
}
[[ "$(sha256sum "$source")" == "$source_before" ]] || {
  echo ".atto guest source changed during AOT verification" >&2; exit 1;
}
[[ -x "$work/guest" && -f "$work/guest" ]] || {
  echo "native output was not executable" >&2; exit 1;
}
# Keep the actual native exit status; nonzero must never be logged as pass.
guest_sha256="$(sha256sum "$work/guest" | awk '{print $1}')"
if timeout --signal=TERM --kill-after=3s 40s "$work/guest"; then
  # Verify compiler, linker and input stayed identical through execution,
  # not only through compilation.
  [[ "$(sha256sum "$compiler")" == "$compiler_before" ]] || {
    echo "LLVM guest compiler changed during native execution" >&2; exit 1;
  }
  [[ "$(sha256sum "$clang_path")" == "$clang_before" ]] || {
    echo "Clang changed during native execution" >&2; exit 1;
  }
  [[ "$(sha256sum "$source")" == "$source_before" ]] || {
    echo ".atto guest source changed during native execution" >&2; exit 1;
  }
  [[ "$(sha256sum "$work/guest" | awk '{print $1}')" == "$guest_sha256" ]] || {
    echo "native guest binary changed during execution" >&2; exit 1;
  }
  printf 'PASS: native LLVM/Clang guest executed 32768-row .atto SoA checksum binary_sha256=%s source_sha256=%s compiler_sha256=%s clang_sha256=%s\n' \
    "$guest_sha256" "${source_before%% *}" "${compiler_before%% *}" "${clang_before%% *}"
else
  status=$?
  echo "FAIL: guest returned nonzero exit status $status" >&2
  exit 1
fi
