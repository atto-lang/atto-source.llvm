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
# Do not follow a tracked symlink into a mutable, untracked build or external
# path. The launcher must be a regular executable Git blob at the pinned SHA.
if [[ ! "$(git -C "$java_root" ls-files --stage -- "$launcher_relative_path")" =~ ^100755[[:space:]] ]]; then
  echo "Java launcher must be a tracked regular executable (mode 100755)" >&2
  exit 65
fi

llvm_sha="$(git -C "$source_root" rev-parse --verify HEAD)"
java_sha="$(git -C "$java_root" rev-parse --verify HEAD)"
require_clean_checkout() {
  local tree
  for tree in "$source_root" "$java_root"; do
    # Includes untracked files: a new .ores import could otherwise change
    # execution without changing the pinned Git SHA or tracked diffs.
    local inventory_status
    if ! inventory_status="$(git -C "$tree" status --porcelain --untracked-files=all)"; then
      echo "Git worktree inventory failed: $tree" >&2
      exit 65
    fi
    if [[ -n "$inventory_status" ]]; then
      echo "source checkout has tracked or untracked changes: $tree" >&2
      exit 65
    fi
    # Git status trusts assume-unchanged/skip-worktree index hints. Those
    # hints can hide tampered source bytes while status reports a clean tree.
    # Reject the hints rather than assigning HEAD's SHA to unchecked bytes.
    git -C "$tree" ls-files -v -z | while IFS= read -r -d '' indexed; do
      if [[ "$indexed" != "H "* ]]; then
        echo "source checkout contains unverified Git index flags: $tree" >&2
        exit 65
      fi
    done || { echo "Git index-flag inventory failed: $tree" >&2; exit 65; }
    # Stat-cache races can still hide worktree edits. Verify the SHA of
    # every tracked regular file, not just git status or selected fixtures.
    # Refuse unpinned symlinks and submodule gitlinks for this strict gate.
    if ! git -C "$tree" diff --no-ext-diff --cached --quiet HEAD --; then
      echo "source checkout Git index diverges from pinned HEAD: $tree" >&2
      exit 65
    fi
    git -C "$tree" ls-files --stage -z | while IFS= read -r -d '' staged; do
      local meta relative mode blob stage actual
      meta="${staged%%$'\t'*}"
      relative="${staged#*$'\t'}"
      read -r mode blob stage <<< "$meta"
      if [[ "$stage" != 0 || ( "$mode" != 100644 && "$mode" != 100755 ) ]]; then
        echo "unverified tracked file mode or merge stage: $tree/$relative" >&2
        exit 65
      fi
      if [[ ! -f "$tree/$relative" || -L "$tree/$relative" ]]; then
        echo "tracked source is not a regular file: $tree/$relative" >&2
        exit 65
      fi
      actual="$(git -C "$tree" hash-object -- "$tree/$relative")"
      if [[ "$actual" != "$blob" ]]; then
        echo "tracked source bytes differ from pinned Git blob: $tree/$relative" >&2
        exit 65
      fi
    done || { echo "Git tracked-blob inventory failed: $tree" >&2; exit 65; }
    # status --untracked-files does not report ignored files. A local ignored
    # .ores import could affect the run without changing either commit SHA.
    # Permit only generated build output in the dedicated build directories.
    git -C "$tree" ls-files --others --ignored --exclude-standard -z | while IFS= read -r -d '' ignored; do
      if [[ "$tree" == "$source_root" && "$tree/$ignored" == "$build_root"/* ]]; then
        continue
      fi
      if [[ "$tree" == "$java_root" && "$ignored" == target/* ]]; then
        continue
      fi
      echo "source checkout contains ignored untracked input: $tree/$ignored" >&2
      exit 65
    done || { echo "Git ignored-input inventory failed: $tree" >&2; exit 65; }
  done
}
require_clean_checkout

# Both repositories must carry the same committed source corpus. Comparing
# outputs alone is insufficient if one repo silently changes its fixtures.
for fixture in main.ores main-1.ores; do
  relative="tests/fixtures/native-class-string/$fixture"
  git -C "$source_root" ls-files --error-unmatch -- "$relative" >/dev/null
  git -C "$java_root" ls-files --error-unmatch -- "$relative" >/dev/null
  for tree in "$source_root" "$java_root"; do
    # cmp follows symlinks. A symlink committed as mode 120000 could resolve
    # outside either checkout, defeating the commit-pinned fixture guarantee.
    if [[ ! "$(git -C "$tree" ls-files --stage -- "$relative")" =~ ^100644[[:space:]] ]]; then
      echo "expected regular tracked guest source fixture (mode 100644): $tree/$relative" >&2
      exit 65
    fi
  done
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
# Independently pin the expected bytes, rather than trusting equal-looking
# stdout in a subprocess-authored receipt. In particular two identical but
# incorrect outputs must not be release-qualified.
expected_stdout_hex_by_case='{"exact-user-program":"68656c6c6f20776f726c640a686969696920326869696969","string-content-equality":"657175616c0a646966666572656e740a656d7074790a","string-equality-embedded-nul-and-unicode":"6e756c0a6c656e6774680a756e69636f64650a","http-string-routing-primitives":"6865616c74680a6d697373696e670a6d6574686f640a","source-effect-order":"6265666f72653b6d6574686f643b61667465723b6d6574686f643b6161","parameters-and-return":"6c65667472696768740a","lazy-conditional":"796573","escaped-unicode":"c3a9f09f98800a09225c","empty-and-embedded-nul":"610062"}'
if ! jq -e -s --arg llvm "$llvm_sha" --arg java "$java_sha" \
  --argjson expected "$expected_stdout_hex_by_case" '
  (length == 1) and (.[0] |
  .schema == "oreslang.java-llvm.source-parity.v1" and
  .status == "passed" and
  .llvm_revision == $llvm and
  .java_revision == $java and
  .positive_programs == 9 and
  .native_executions == 45 and
  .java_executions == 18 and
  .llvm_only_negative_rejections >= 16 and
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
    (.native_stdout_hex == $expected[.name]) and
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
