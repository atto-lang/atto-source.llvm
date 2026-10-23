#!/usr/bin/env bash
# Exact-commit native class/String compiler smoke on macOS.
# Usage: bash scripts/verify-native-class-string-macos.sh [19|23]
set -euo pipefail
version="${1:-23}"
case "$version" in 19) formula="llvm@19";; 23) formula="llvm";; *) echo "Choose LLVM 19 or 23." >&2; exit 64;; esac
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
cd "$root"
command -v brew >/dev/null || { echo "Requires Homebrew on macOS" >&2; exit 69; }
command -v rustc >/dev/null || { echo "Requires Rust toolchain" >&2; exit 69; }
prefix="$(brew --prefix "$formula")"
found="$("$prefix/bin/llvm-config" --version)"
[[ "$found" == "$version".* ]] || { echo "Expected LLVM $version, found $found" >&2; exit 65; }
# The printed commit/tree receipt is only evidence for a clean source tree.
# Ignore generated build directories but reject edits or untracked files in
# source/build input paths. The compiler must not compile an unrecorded patch.
if [[ -n "$(git status --porcelain --untracked-files=all -- \
    CMakeLists.txt src/main/cpp/oreslang tests/native_class_string_parity.rs \
    tests/fixtures/native-class-string scripts/compile-aot.sh)" ]]; then
  echo "Refusing dirty build inputs: commit or discard changes before verification" >&2
  exit 65
fi
printf "Source commit: %s\nSource tree: %s\nLLVM: %s\n" "$(git rev-parse HEAD)" "$(git rev-parse HEAD^{tree})" "$found"
build="$root/build-class-string-llvm$version"
cmake -S . -B "$build" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
  -DLLVM_DIR="$prefix/lib/cmake/llvm" -DORE_CLANG="$prefix/bin/clang"
cmake --build "$build" --target oreslang-llvmc oreslang-native-class-string-parity-build --parallel 2
ctest --test-dir "$build" -R "^native_class_string_parity_release$" --output-on-failure --no-tests=error
if [[ "$version" == 23 ]]; then
  ctest --test-dir "$build" -R "^native_class_string_parity_sanitizer$" --output-on-failure --no-tests=error
fi
work="$(mktemp -d "${TMPDIR:-/tmp}/oreslang-native-text.XXXXXXXX")"
trap 'rm -rf -- "$work"' EXIT
cp tests/fixtures/native-class-string/main-1.ores "$work/main-1.ores"
cat > "$work/main.ores" <<'ORES'
import class Foo from "./main-1.ores";
pub routine main(): int {
  stdio.println("hello world");
  val Foo f = new Foo();
  val string x = f.bar();
  stdio.println("x", x);
  return 0;
}
ORES
ORESLANG_LLVMC="$build/oreslang-llvmc" CLANG="$prefix/bin/clang" \
  bash scripts/compile-aot.sh "$work/main.ores" "$work/main-native"
printf "hello world\nxhiiii 2hiiii\n" > "$work/expected"
"$work/main-native" | cat > "$work/output"
diff -u "$work/expected" "$work/output"
echo "PASS: native Foo program through LLVM $found"
