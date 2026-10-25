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
# Keep the receipt tied to actual source bytes, not just git status:
# --assume-unchanged and --skip-worktree can conceal modified tracked files.
# Inputs must be ordinary tracked blobs at exactly this HEAD both before and
# after execution. Generated build directories are intentionally excluded.
input_paths=(
  CMakeLists.txt
  src/main/cpp/oreslang
  tests/native_class_string_parity.rs
  tests/native_source_input_admission.cpp
  tests/fixtures/native-class-string
  scripts/compile-aot.sh
  scripts/verify-native-class-string-macos.sh
  .github/workflows/llvm19-23-native-class-string-macos.yml
)
verify_clean_inputs() {
  local phase="$1" indexed path expected actual count=0
  if [[ -n "$(git status --porcelain --untracked-files=all -- "${input_paths[@]}")" ]]; then
    echo "Refusing modified/untracked build inputs ($phase)" >&2
    exit 65
  fi
  while IFS= read -r -d '' indexed; do
    # -v prints H for ordinary index entries, h for assume-unchanged,
    # and S for skip-worktree. Refuse any nonstandard index state.
    if [[ "${indexed:0:1}" != H ]]; then
      echo "Refusing hidden index state ($phase): ${indexed:2}" >&2
      exit 65
    fi
    path="${indexed:2}"
    expected="$(git rev-parse --verify "HEAD:$path")" || exit 65
    actual="$(git hash-object -- "$path")" || exit 65
    if [[ "$actual" != "$expected" ]]; then
      echo "Refusing source blob mismatch ($phase): $path" >&2
      exit 65
    fi
    count=$((count + 1))
  done < <(git ls-files -z -v -- "${input_paths[@]}")
  if (( count < 24 )); then
    echo "Refusing incomplete tracked input inventory ($phase): $count" >&2
    exit 65
  fi
}
verify_clean_inputs preflight
printf "Source commit: %s\nSource tree: %s\nLLVM: %s\n" "$(git rev-parse HEAD)" "$(git rev-parse HEAD^{tree})" "$found"
build="$root/build-class-string-llvm$version"
cmake -S . -B "$build" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
  -DLLVM_DIR="$prefix/lib/cmake/llvm" -DORE_CLANG="$prefix/bin/clang"
cmake --build "$build" --target oreslang-llvmc oreslang-native-class-string-parity-build oreslang-native-source-input-admission-test --parallel 2
# Exercise the source-file and import admission boundary in this same
# exact-commit matrix, not only in isolated developer-side CTest runs.
# A release pass on LLVM 19 cannot stand in for ASan/UBSan qualification.
ctest --test-dir "$build" \
  -R "^native_(class_string_parity_(release|sanitizer)|source_input_admission)$" \
  --output-on-failure --no-tests=error
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
verify_clean_inputs postflight
echo "PASS: native Foo program through LLVM $found"
