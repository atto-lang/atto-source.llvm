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
0dedccfdeff013995e1e5d784bf1767acf61b0c0485fa50aba6aa9dd473331f2  tests/fixtures/opto-sync-scalar-v1/scalar-order.ores
c1be02a97cbf9c02a3ff6ed2c3452cbde2e7adca77b03c93ea2a185b87fd5a8f  tests/fixtures/opto-sync-scalar-v1/scalar-parens.ores
48e5c6d9a4284d5d9c05bab779fa74a53341a981d5e6c63ce6cc5c52cbf323dd  tests/fixtures/opto-sync-scalar-v1/scalar-boundary.ores
dbc6368f793beef3465a061983134ef85621d9084bc179f8d53c096fa0242461  tests/fixtures/opto-sync-scalar-v1/reject-overflow.ores
dce35f88681548d3e3e4e1d04394924a9b70f27925acc37a53689d74237ec33a  tests/fixtures/opto-sync-scalar-v1/reject-invalid.ores
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
