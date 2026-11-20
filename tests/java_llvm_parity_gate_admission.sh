#!/usr/bin/env bash
# Synthetic git trees ONLY. This validates fail-closed qualification admission,
# not Java/LLVM source compilation, execution, or any guest parity.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd -P)"
temp="$(mktemp -d)"
trap 'rm -rf "$temp"' EXIT
mkdir -p "$temp/build"
runner="$temp/llvm/scripts/verify-java-llvm-source-parity.sh"

make_repos() {
  rm -rf "$temp/llvm" "$temp/java"
  for kind in llvm java; do
    mkdir -p "$temp/$kind/tests/fixtures/native-class-string" "$temp/$kind/scripts"
    printf 'fnc main():int{return 0;}\n' > "$temp/$kind/tests/fixtures/native-class-string/main.ores"
    printf 'define pub class Foo as end\n' > "$temp/$kind/tests/fixtures/native-class-string/main-1.ores"
  done
  mkdir -p "$temp/llvm/src/main/cpp/oreslang/runtime"
  printf '/* admission-only placeholder, never compiled */\n' > "$temp/llvm/src/main/cpp/oreslang/runtime/NativeStringRegion.c"
  cp "$root/scripts/verify-java-llvm-source-parity.sh" "$runner"
  printf '#!/bin/sh\nexit 0\n' > "$temp/java/scripts/run-source-parity.sh"
  chmod +x "$temp/java/scripts/run-source-parity.sh"
  for kind in llvm java; do
    git -C "$temp/$kind" init -q
    git -C "$temp/$kind" add .
    git -C "$temp/$kind" -c user.name=ParityAdmission -c user.email=parity@invalid.example commit -qm initial
  done
}

expect_rejected() {
  local label="$1" status="$2" diagnostic="$3" launcher="$4"
  local observed=0
  bash "$runner" "$temp/build" "$temp/java" "$launcher" > "$temp/stdout" 2> "$temp/stderr" || observed="$?"
  if [[ "$observed" != "$status" ]] || ! grep -Fq "$diagnostic" "$temp/stderr"; then
    echo "FAIL $label: expected code=$status and diagnostic=$diagnostic; got=$observed" >&2
    cat "$temp/stderr" >&2
    exit 1
  fi
  if [[ -e "$temp/build/java-llvm-source-parity.json" ]]; then
    echo "FAIL $label: denied gate emitted a success receipt" >&2
    exit 1
  fi
  echo "PASS admission: $label"
}

bash -n "$root/scripts/verify-java-llvm-source-parity.sh"
make_repos
launcher="$temp/java/scripts/run-source-parity.sh"
usage_code=0
bash "$runner" "$temp/build" >"$temp/stdout" 2>"$temp/stderr" || usage_code="$?"
if [[ "$usage_code" != 64 ]] || ! grep -Fq 'usage:' "$temp/stderr"; then
  echo "FAIL usage: expected exit 64 and usage diagnostic, got $usage_code" >&2
  exit 1
fi
echo "PASS admission: incomplete CLI arguments"

make_repos
printf 'untracked inject\n' > "$temp/java/injected.ores"
expect_rejected "untracked Java import" 65 "source checkout has tracked or untracked changes" "$launcher"

make_repos
printf 'untracked inject\n' > "$temp/llvm/injected.ores"
expect_rejected "untracked LLVM import" 65 "source checkout has tracked or untracked changes" "$launcher"

make_repos
printf 'changed\n' > "$temp/java/tests/fixtures/native-class-string/main-1.ores"
git -C "$temp/java" add .
git -C "$temp/java" -c user.name=ParityAdmission -c user.email=parity@invalid.example commit -qm new-fixture
expect_rejected "committed fixture divergence" 65 "source fixtures diverge" "$launcher"

make_repos
printf '#!/bin/sh\nexit 0\n' > "$temp/java/scripts/new-launcher.sh"
chmod +x "$temp/java/scripts/new-launcher.sh"
expect_rejected "untracked launcher" 65 "launcher must be a tracked file" "$temp/java/scripts/new-launcher.sh"

make_repos
printf 'local mutation\n' >> "$temp/java/tests/fixtures/native-class-string/main.ores"
expect_rejected "tracked source mutation" 65 "source checkout has tracked or untracked changes" "$launcher"

# Two committed symlinks to the same external file can pass cmp byte-equality
# while their contents are not covered by either reported Git commit.
make_repos
printf 'pub fnc main():int{return 0;}\n' > "$temp/external-main.ores"
for kind in llvm java; do
  rm "$temp/$kind/tests/fixtures/native-class-string/main.ores"
  ln -s "$temp/external-main.ores" "$temp/$kind/tests/fixtures/native-class-string/main.ores"
  git -C "$temp/$kind" add tests/fixtures/native-class-string/main.ores
  git -C "$temp/$kind" -c user.name=ParityAdmission -c user.email=parity@invalid.example commit -qm symlink-fixture
done
expect_rejected "committed external fixture symlinks" 65 "expected regular tracked guest source fixture" "$launcher"

# git status can report a clean checkout when assume-unchanged hides a
# modified tracked fixture. The gate must reject that index state regardless.
make_repos
git -C "$temp/java" update-index --assume-unchanged tests/fixtures/native-class-string/main.ores
printf 'tampered source\n' > "$temp/java/tests/fixtures/native-class-string/main.ores"
if [[ -n "$(git -C "$temp/java" status --porcelain --untracked-files=all)" ]]; then
  echo "FAIL setup: hidden tracked mutation unexpectedly visible to git status" >&2
  exit 1
fi
expect_rejected "hidden tracked Java fixture mutation" 65 "unverified Git index flags" "$launcher"

# A skip-worktree bit can hide edited files from Git porcelain, much like
# assume-unchanged. This is especially dangerous for compiler inputs and must
# fail closed even if Git reports no modifications.
make_repos
git -C "$temp/llvm" update-index --skip-worktree tests/fixtures/native-class-string/main-1.ores
printf 'tampered source via skip-worktree\n' > "$temp/llvm/tests/fixtures/native-class-string/main-1.ores"
if [[ -n "$(git -C "$temp/llvm" status --porcelain --untracked-files=all)" ]]; then
  echo "FAIL setup: skip-worktree mutation unexpectedly visible to git status" >&2
  exit 1
fi
expect_rejected "skip-worktree LLVM fixture mutation" 65 "unverified Git index flags" "$launcher"

# Process substitution discards the producing Git process's exit status.
# The strict gate must reject inventory command failures even on a clean tree.
mkdir -p "$temp/mockgit"
actual_git="$(command -v git)"
cat > "$temp/mockgit/git" <<'MOCK_GIT'
#!/usr/bin/env bash
set -euo pipefail
if [[ "${1-}" == "-C" && "${3-}" == "status" && "${FAIL_LSFILES_KIND:-}" == "status" ]]; then
  exit 75
fi
if [[ "${1-}" == "-C" && "${3-}" == "ls-files" ]]; then
  case "${FAIL_LSFILES_KIND:-}" in
    flags)
      if [[ "${4-}" == "-v" && "${5-}" == "-z" ]]; then exit 72; fi ;;
    blobs)
      if [[ "${4-}" == "--stage" && "${5-}" == "-z" ]]; then exit 73; fi ;;
    ignored)
      if [[ "${4-}" == "--others" && "${5-}" == "--ignored" ]]; then exit 74; fi ;;
  esac
fi
exec "$ACTUAL_GIT" "$@"
MOCK_GIT
chmod +x "$temp/mockgit/git"
make_repos
ACTUAL_GIT="$actual_git" FAIL_LSFILES_KIND=status PATH="$temp/mockgit:$PATH" expect_rejected "failed worktree status" 65 "Git worktree inventory failed" "$launcher"
make_repos
ACTUAL_GIT="$actual_git" FAIL_LSFILES_KIND=flags PATH="$temp/mockgit:$PATH" expect_rejected "failed index flag scan" 65 "Git index-flag inventory failed" "$launcher"
make_repos
ACTUAL_GIT="$actual_git" FAIL_LSFILES_KIND=blobs PATH="$temp/mockgit:$PATH" expect_rejected "failed tracked blob scan" 65 "Git tracked-blob inventory failed" "$launcher"
make_repos
ACTUAL_GIT="$actual_git" FAIL_LSFILES_KIND=ignored PATH="$temp/mockgit:$PATH" expect_rejected "failed ignored input scan" 65 "Git ignored-input inventory failed" "$launcher"

# --untracked-files=all still omits Git-ignored sources; imported ignored
# Oreslang code may not be credited to the pinned source revision.
make_repos
printf 'injected.ores\n' >> "$temp/java/.git/info/exclude"
printf 'pub fnc main():int{return 1;}\n' > "$temp/java/injected.ores"
expect_rejected "ignored untracked Java source" 65 "ignored untracked input" "$launcher"

make_repos
printf 'injected.ores\n' >> "$temp/llvm/.git/info/exclude"
printf 'pub fnc main():int{return 1;}\n' > "$temp/llvm/injected.ores"
expect_rejected "ignored untracked LLVM source" 65 "ignored untracked input" "$launcher"

# No tracked symlink may appear anywhere in a source-exact checkout, even if
# it is not one of the two explicitly compared guest fixtures.
make_repos
ln -s "tests/fixtures/native-class-string/main.ores" "$temp/java/unrelated-symlink.ores"
git -C "$temp/java" add unrelated-symlink.ores
git -C "$temp/java" -c user.name=ParityAdmission -c user.email=parity@invalid.example commit -qm unrelated-symlink
expect_rejected "unrelated committed source symlink" 65 "unverified tracked file mode" "$launcher"

echo "PASS: synthetic fail-closed parity gate admissions; no runtime parity inferred"
