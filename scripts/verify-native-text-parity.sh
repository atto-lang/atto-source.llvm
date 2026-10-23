#!/usr/bin/env bash
# Real source-to-IR-to-native execution gate for installed macOS LLVM toolchains.
# Usage: bash scripts/verify-native-text-parity.sh [19] [23]
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
cd "$root"

if ! command -v brew >/dev/null 2>&1; then
  echo "This gate selects Homebrew LLVM. Run equivalent CMake/CTest commands elsewhere." >&2
  exit 69
fi
if [[ $# == 0 ]]; then set -- 19 23; fi
for version in "$@"; do
  case "$version" in
    19) formula="llvm@19" ;;
    23) formula="llvm" ;;
    *) echo "unsupported requested verification version: $version" >&2; exit 64 ;;
  esac

  if ! prefix="$(brew --prefix "$formula" 2>/dev/null)" || [[ ! -x "$prefix/bin/clang" ]]; then
    echo "Missing $formula; install with: brew install $formula" >&2
    exit 69
  fi

  found="$("$prefix/bin/llvm-config" --version)"
  if [[ "$found" != "$version".* ]]; then
    echo "Wrong LLVM toolchain: requested $version, found $found ($formula)" >&2
    exit 65
  fi

  build="$root/build-llvm${version}-text-parity"
  echo "Verifying source tree $(git rev-parse HEAD) with LLVM $found"
  cmake -S "$root" -B "$build" -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_TESTING=ON -DLLVM_DIR="$prefix/lib/cmake/llvm"
  cmake --build "$build" --target oreslang-llvmc --parallel 2

  # Run the exact compiler/Clang pair. CTest also exercises all negative cases.
  bash "$root/tests/native_text_class_import_smoke.sh" \
      "$build/oreslang-llvmc" "$prefix/bin/clang" \
      "$root/tests/fixtures/native-text-class/main.ores"
  echo "PASS: native text/class source smoke on LLVM $found"
done
