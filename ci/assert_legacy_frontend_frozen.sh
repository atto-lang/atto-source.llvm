#!/usr/bin/env bash
# Frozen legacy Rust browser-to-Wasm compiler snapshot: NO new frontend.
# Canonical Rust compiler ownership is atto-lang/atto-source.rs.
# Temporary migration guard until the Rust LLVM adapter is validated at #468
# and this directory is removed from the native LLVM repository.
set -euo pipefail
cd "$(dirname "$0")/.."
expected_count=16
mapfile -t tracked < <(git ls-files -- frontend)
if (( ${#tracked[@]} != expected_count )); then
  echo "FROZEN_FRONTEND_MISMATCH: expected ${expected_count} tracked files, found ${#tracked[@]}" >&2
  exit 1
fi
while read -r expected_sha path; do
  [[ "$expected_sha" =~ ^[0-9a-f]{40}$ ]] || exit 1
  if [[ ! -f "$path" || -L "$path" ]]; then
    echo "FROZEN_FRONTEND_MISMATCH: missing/nonregular $path" >&2
    exit 1
  fi
  if [[ "$(git ls-files --stage -- "$path" | cut -f1 | cut -d' ' -f1)" != "100644" ]]; then
    echo "FROZEN_FRONTEND_MISMATCH: unexpected mode $path" >&2
    exit 1
  fi
  actual_sha=$(git hash-object -- "$path")
  if [[ "$actual_sha" != "$expected_sha" ]]; then
    echo "FROZEN_FRONTEND_MISMATCH: source copy drift $path" >&2
    exit 1
  fi
done <<'PINNED_FRONTEND_BLOBS'
387853d3838d48e0c89d77fbecfcd41d53fbd4ca frontend/Cargo.lock
7bcb27474e87c1f05d6f96acf8a6a6c75ac4bcef frontend/Cargo.toml
7f9a6fd6218361aed668983edf73b7100faf5ec6 frontend/src/ast.rs
fc8806e97d94f408fd145f48b73ab33324dae2fa frontend/src/check.rs
58248b23c28486a220f501f5826b59f3edf88499 frontend/src/check/generator_lower.rs
c568ea9cb79f6f6f05ea722440f7ac2a9cdeeb4f frontend/src/check/generator_lower/liveness.rs
b9a5b982251315604e90680066bbbbfc49ab1a2a frontend/src/diag.rs
f223acff454541b1a08e1968ab0f07a4a516910b frontend/src/generator_cfg.rs
28e20b83c50f3c8926930ec34b567b699371a377 frontend/src/generator_frame.rs
23f8fe950b47627110cf3265b771398bb46213e3 frontend/src/lexer.rs
c2b0e3facec7d12a8a5dccf7401bc8ab48ab0427 frontend/src/lib.rs
6231be661be9b7bb40bd9ed69d4cd60ccc5b9609 frontend/src/parser.rs
d3e368e6fa3ed855cd376909120bd11691013afd frontend/src/target_syntax.rs
ae94ba2232ce26f7da37adcbf77d9ba800f33f63 frontend/src/target_typed.rs
4387f655111e4484d071192b38fdd4f8a39c5683 frontend/src/wasm.rs
bbf30945334f11904783f438343e989e78e08e75 frontend/src/wasm/generator_runtime.rs
PINNED_FRONTEND_BLOBS
echo "Frozen legacy frontend: ${expected_count} byte-exact Git blobs; no canonical frontend in .llvm"
