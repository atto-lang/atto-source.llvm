#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C
ulimit -c 0 || true
if [[ $# -ne 2 ]]; then
  echo "usage: $0 <oreslang-llvmc> <clang>" >&2
  exit 2
fi
compiler="$1"
clang="$2"
fixtures="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/fixtures/logical-punctuation-v1"
tmp="$(mktemp -d "${TMPDIR:-/tmp}/atto-punct-ops.XXXXXX")"
trap 'rm -rf "$tmp"' EXIT
for executable in "$compiler" "$clang"; do
  if [[ ! -x "$executable" ]]; then
    echo "compiler or clang not executable: $executable" >&2
    exit 2
  fi
done
command -v timeout >/dev/null || { echo "timeout command is required" >&2; exit 2; }
ir="$tmp/positive.ll"
if ! timeout 40s "$compiler" --target=host "$fixtures/scalar-parity.atto" > "$ir"; then
  echo "native compiler rejected positive .atto source" >&2
  exit 1
fi
if [[ ! -s "$ir" ]]; then
  echo "native compiler emitted empty IR" >&2
  exit 1
fi
for opt in 0 2; do
  binary="$tmp/native-O$opt"
  timeout 40s "$clang" "-O$opt" -x ir "$ir" -o "$binary"
  status=0
  timeout 20s "$binary" >"$tmp/native-O$opt.stdout" 2>"$tmp/native-O$opt.stderr" || status=$?
  if [[ -s "$tmp/native-O$opt.stdout" ]]; then
    echo "native guest emitted unexpected stdout at O$opt" >&2
    exit 1
  fi
  if [[ "$status" -ne 42 ]]; then
    echo "native guest returned $status at O$opt, expected 42" >&2
    exit 1
  fi
done
# Lowercase operator words remain ordinary user identifiers (not keywords).
identifier_ir="$tmp/keyword-identifiers.ll"
timeout 40s "$compiler" --target=host "$fixtures/keyword-identifiers.atto" > "$identifier_ir"
if [[ ! -s "$identifier_ir" ]]; then
  echo "compiler emitted empty IR for keyword identifiers" >&2
  exit 1
fi
for opt in 0 2; do
  binary="$tmp/identifiers-O$opt"
  timeout 40s "$clang" "-O$opt" -x ir "$identifier_ir" -o "$binary"
  status=0
  timeout 20s "$binary" >"$tmp/identifiers-O$opt.stdout" 2>"$tmp/identifiers-O$opt.stderr" || status=$?
  if [[ -s "$tmp/identifiers-O$opt.stdout" ]]; then
    echo "identifier guest emitted unexpected stdout at O$opt" >&2
    exit 1
  fi
  if [[ "$status" -ne 42 ]]; then
    echo "identifier fixture returned $status at O$opt, expected 42" >&2
    exit 1
  fi
done
for name in reject-int-and reject-int-or reject-int-xor reject-single-caret reject-spaced-caret reject-bare-pipe reject-bare-ampersand; do
  out="$tmp/$name.ll"
  diagnostic="$tmp/$name.stderr"
  status=0
  timeout 40s "$compiler" --target=host "$fixtures/$name.atto" >"$out" 2>"$diagnostic" || status=$?
  if [[ "$status" -eq 0 ]]; then
    echo "native compiler accepted invalid boolean punctuation fixture $name" >&2
    exit 1
  fi
  if [[ "$status" -eq 124 || "$status" -eq 125 || "$status" -eq 126 || "$status" -eq 127 ]]; then
    echo "compiler timed out or was unavailable (not a valid compile-negative): $name, status=$status" >&2
    exit 1
  fi
  if [[ -s "$out" ]]; then
    echo "compiler emitted executable IR after rejection: $name" >&2
    exit 1
  fi
  if [[ ! -s "$diagnostic" ]]; then
    echo "compiler provided no diagnostic for $name" >&2
    exit 1
  fi
done
echo "logical punctuation: native O0/O2 + compile negatives passed"
