#!/usr/bin/env bash
# Strict source-level Java/GraalVM <-> LLVM differential gate. No silent skip.
# Usage: bash scripts/verify-java-llvm-source-parity.sh BUILD_DIR JAVA_CHECKOUT JAVA_LAUNCHER [CLANG]
set -euo pipefail

if (( $# < 3 || $# > 4 )); then
  echo "usage: $0 BUILD_DIR JAVA_CHECKOUT JAVA_LAUNCHER [CLANG]" >&2
  exit 64
fi
source_root="$(cd "$(dirname "$0")/.." && pwd -P)"
build_root="$(cd "$1" && pwd -P)"
java_root="$(cd "$2" && pwd -P)"
java_launcher="$(realpath "$3")"
clang="clang"
if (( $# == 4 )); then clang="$4"; fi

# A pinned checkout must own the launcher, not an unrelated Java executable.
case "$java_launcher" in
  "$java_root"/*) ;;
  *) echo "Java launcher must resolve inside the pinned Java checkout" >&2; exit 65 ;;
esac

if [[ ! -x "$java_launcher" ]]; then
  echo "missing executable Java source launcher: $java_launcher" >&2
  exit 66
fi
if [[ ! -f "$source_root/src/main/cpp/oreslang/runtime/NativeStringRegion.c" ]]; then
  echo "native String runtime is missing" >&2
  exit 66
fi
if ! command -v "$clang" >/dev/null 2>&1; then
  echo "Clang is required: $clang" >&2
  exit 66
fi
if ! command -v git >/dev/null 2>&1; then
  echo "git is required for exact-revision evidence" >&2
  exit 66
fi
# The launcher itself must be tracked by the Java revision being reported.
launcher_relative_path="${java_launcher#"$java_root"/}"
if ! git -C "$java_root" ls-files --error-unmatch -- "$launcher_relative_path" >/dev/null 2>&1; then
  echo "Java launcher must be a tracked file at the pinned Java revision" >&2
  exit 65
fi

llvm_sha="$(git -C "$source_root" rev-parse --verify HEAD)"
java_sha="$(git -C "$java_root" rev-parse --verify HEAD)"
for tree in "$source_root" "$java_root"; do
  git -C "$tree" diff --quiet --exit-code
  git -C "$tree" diff --cached --quiet --exit-code
done

# Rebuild both implementations from the exact pinned source trees; a stale
# compiler/guest runtime may not be credited to the source revisions.
for tool in cmake mvn; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "required build tool is missing: $tool" >&2
    exit 66
  fi
done
cmake -S "$source_root" -B "$build_root" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build "$build_root" --parallel 2 --target oreslang-llvmc oreslang-native-class-string-parity-build
(
  cd "$java_root"
  mvn -B -ntp -DskipTests clean package
  mvn -B -ntp -q dependency:build-classpath -Dmdep.outputFile=target/conformance-classpath.txt
)
for required in "$build_root/oreslang-llvmc" "$build_root/oreslang-native-class-string-parity"; do
  if [[ ! -x "$required" ]]; then
    echo "missing executable after pinned source build: $required" >&2
    exit 66
  fi
done

report="$build_root/java-llvm-source-parity.json"
if [[ -e "$report" || -L "$report" ]]; then
  echo "refusing to overwrite an existing parity receipt: $report" >&2
  exit 73
fi

staging="$build_root/.java-llvm-source-parity.pending.$RANDOM.$PPID"
if [[ -e "$staging" || -L "$staging" ]]; then
  echo "refusing to reuse a stale parity staging file: $staging" >&2
  exit 73
fi
trap 'rm -f "$staging"' EXIT

"$build_root/oreslang-native-class-string-parity" \
  --compiler "$build_root/oreslang-llvmc" \
  --clang "$clang" \
  --runtime "$source_root/src/main/cpp/oreslang/runtime/NativeStringRegion.c" \
  --java-command "$java_launcher" \
  --require-java \
  --llvm-revision "$llvm_sha" \
  --java-revision "$java_sha" \
  --report "$staging"

# Reject a checkout that moved or became dirty during the run.
[[ "$llvm_sha" == "$(git -C "$source_root" rev-parse --verify HEAD)" ]]
[[ "$java_sha" == "$(git -C "$java_root" rev-parse --verify HEAD)" ]]
for tree in "$source_root" "$java_root"; do
  git -C "$tree" diff --quiet --exit-code
  git -C "$tree" diff --cached --quiet --exit-code
done
test -s "$staging"
# Atomic no-clobber publish only after Java/LLVM source integrity is rechecked.
ln "$staging" "$report"
rm "$staging"
trap - EXIT
echo "Source-level Java/LLVM parity receipt: $report"
