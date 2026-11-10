#!/usr/bin/env bash
# Real LLVM compiler -> Clang native executable -> process exit. NOT host-only.
set -Eeuo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
cd "$root"
compiler="$root/build/oreslang-llvmc"
[[ -f "$compiler" && -x "$compiler" && ! -L "$compiler" ]] || { echo 'real oreslang-llvmc missing' >&2; exit 2; }
command -v clang >/dev/null || { echo 'clang missing' >&2; exit 2; }
echo "SOURCE_HEAD=$(git rev-parse HEAD)"
echo "SOURCE_TREE=$(git rev-parse 'HEAD^{tree}')"
echo "OPTO_SYNC_LLVM_COMPILER_SHA256=$(sha256sum "$compiler" | cut -d' ' -f1)"
sha256sum --check --status <<'CHECKSUMS'
dca8db9791954aa794931e002fa35433d8f88075e433ec7559455bc6e8be7d06  tests/fixtures/opto-sync-scalar-v1/scalar-order.ores
c308ce14123d092be7614794b8307e492056ea248dfd5f211151b19e767290ac  tests/fixtures/opto-sync-scalar-v1/scalar-parens.ores
5ded4f6c37b81c9b072e17acb4be852d5be976edd594d20c76edc5a5ed9d69aa  tests/fixtures/opto-sync-scalar-v1/scalar-boundary.ores
1c1b1959a3b80ff4aebb97fdf18a80a02540c7fc8ecb8d3b3b953aadaf6b95f0  tests/fixtures/opto-sync-scalar-v1/reject-overflow.ores
da78107d4ba848d2f27c3f6b0a4b5c6b191a137c47f25cd44010d8c51bed0ebc  tests/fixtures/opto-sync-scalar-v1/reject-invalid.ores
CHECKSUMS
src=tests/fixtures/opto-sync-scalar-v1
work="$(mktemp -d "$root/build/opto-sync-scalar.XXXXXXXX")"
trap 'rm -rf -- "$work"' EXIT
for pair in scalar-order:14 scalar-parens:42 scalar-boundary:1; do
  id=${pair%%:*}
  expected=${pair#*:}
  binary="$work/$id"
  bash scripts/compile-aot.sh "$src/$id.ores" "$binary" >"$work/compiler.stdout" 2>"$work/compiler.stderr"
  [[ -f "$binary" && -x "$binary" && ! -L "$binary" ]] || { echo "compiler emitted no native guest: $id" >&2; exit 1; }
  artifact_sha=$(sha256sum "$binary" | cut -d' ' -f1)
  if "$binary" >"$work/stdout" 2>"$work/stderr"; then exit_code=0; else exit_code=$?; fi
  [[ "$exit_code" = "$expected" ]] || { echo "wrong real native guest exit: $id expected=$expected actual=$exit_code" >&2; exit 1; }
  [[ ! -s "$work/stdout" && ! -s "$work/stderr" ]] || { echo "unexpected native guest IO: $id" >&2; exit 1; }
  echo "OPTO_SYNC_ATTO_LLVM_REAL_GUEST id=$id expected_exit=$expected artifact_sha256=$artifact_sha"
done
for id in reject-overflow reject-invalid; do
  ir="$work/$id.ll"
  binary="$work/$id"
  if "$compiler" --target=host "$src/$id.ores" >"$ir" 2>"$work/deny.stderr"; then
    echo "compiler admitted invalid Opto Sync source: $id" >&2
    exit 1
  fi
  [[ ! -s "$ir" && -s "$work/deny.stderr" ]] || { echo "invalid source published IR or no diagnostic: $id" >&2; exit 1; }
  if grep -Eqi 'segmentation fault|stack trace|panic|internal compiler error|assertion failed' "$work/deny.stderr"; then
    echo "compiler crashed rather than semantically denying $id" >&2
    exit 1
  fi
  if bash scripts/compile-aot.sh "$src/$id.ores" "$binary" >"$work/reject.stdout" 2>"$work/reject.stderr"; then
    echo "compiler linked rejected source: $id" >&2
    exit 1
  fi
  [[ ! -e "$binary" && ! -L "$binary" ]] || { echo "invalid source left native artifact: $id" >&2; exit 1; }
  echo "OPTO_SYNC_ATTO_LLVM_COMPILER_REJECTION id=$id"
done
echo 'OPTO_SYNC_ATTO_LLVM_EXACT_GUEST_PASS positives=3 negatives=2'
