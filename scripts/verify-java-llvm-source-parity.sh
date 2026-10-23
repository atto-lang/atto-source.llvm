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
require_clean_checkout() {
  local tree
  for tree in "$source_root" "$java_root"; do
    # Includes untracked files: a new .ores import could otherwise change
    # execution without changing the pinned Git SHA or tracked diffs.
    if [[ -n "$(git -C "$tree" status --porcelain --untracked-files=all)" ]]; then
      echo "source checkout has tracked or untracked changes: $tree" >&2
      exit 65
    fi
  done
}
require_clean_checkout

# Both repositories must carry the same committed source corpus. Comparing
# outputs alone is insufficient if one repo silently changes its fixtures.
for fixture in main.ores main-1.ores; do
  relative="tests/fixtures/native-class-string/$fixture"
  git -C "$source_root" ls-files --error-unmatch -- "$relative" >/dev/null
  git -C "$java_root" ls-files --error-unmatch -- "$relative" >/dev/null
  if ! cmp -s "$source_root/$relative" "$java_root/$relative"; then
    echo "Java and LLVM class/String source fixtures diverge: $relative" >&2
    exit 65
  fi
done

# Rebuild both implementations from the exact pinned source trees; a stale
# compiler/guest runtime may not be credited to the source revisions.
for tool in cmake mvn jq; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "required build tool is missing: $tool" >&2
    exit 66
  fi
done
cmake -S "$source_root" -B "$build_root" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build "$build_root" --clean-first --parallel 2 --target oreslang-llvmc oreslang-native-class-string-parity-build
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
require_clean_checkout
if [[ ! -f "$staging" || -L "$staging" || ! -s "$staging" ]]; then
  echo "qualification did not produce a regular, nonempty receipt" >&2
  exit 65
fi
# An exit-zero subprocess, a nonempty file or a self-reported PASS are not
# enough. Independently check provenance, corpus completeness and binary
# parity before publishing a durable receipt.
if ! jq -e -s --arg llvm "$llvm_sha" --arg java "$java_sha" '
  (length == 1) and (.[0] |
  .schema == "oreslang.java-llvm.source-parity.v1" and
  .status == "passed" and
  .llvm_revision == $llvm and
  .java_revision == $java and
  .positive_programs == 9 and
  .native_executions == 45 and
  .java_executions == 18 and
  .llvm_only_negative_rejections == 16 and
  .sanitized == false and
  ((.cases | type) == "array") and
  ((.cases | length) == 9) and
  ((.cases | map(.name) | sort) == ([
    "exact-user-program", "string-content-equality",
    "string-equality-embedded-nul-and-unicode",
    "http-string-routing-primitives", "source-effect-order",
    "parameters-and-return", "lazy-conditional",
    "escaped-unicode", "empty-and-embedded-nul"
  ] | sort)) and
  all(.cases[];
    .native_exit == 0 and .java_exit == 0 and
    .native_runs == 5 and .java_runs == 2 and
    ((.native_stdout_hex | type) == "string") and
    ((.java_stdout_hex | type) == "string") and
    (.native_stdout_hex == .java_stdout_hex) and
    (.native_stdout_hex | test("^([0-9a-f]{2})*$"))
  ))
' "$staging" >/dev/null; then
  echo "invalid strict parity receipt; refusing release qualification" >&2
  exit 65
fi
# Atomic no-clobber publish only after Java/LLVM source integrity is rechecked.
ln "$staging" "$report"
rm "$staging"
trap - EXIT
echo "Source-level Java/LLVM parity receipt: $report"
