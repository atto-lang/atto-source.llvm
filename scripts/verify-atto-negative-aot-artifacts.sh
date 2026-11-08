#!/usr/bin/env bash
# Native AOT negative artifact gate. --self-test tests guards, not a compiler.
set -euo pipefail
export LC_ALL=C
die() { printf 'ATTO_AOT_NEGATIVE_DENIED: %s\n' "$*" >&2; exit 1; }
# GNU timeout returns 124 for deadline, 125 error, 126/127 exec failure.
# None are compiler-semantic rejections; neither are crash/kill signals.
semantic_failure_exit() {
  [[ "$1" =~ ^[1-9][0-9]{0,2}$ ]] || return 1
  (( 10#$1 <= 123 ))
}
no_artifacts() {
  local dir="$1" entries
  [[ -d "$dir" && ! -L "$dir" ]] || return 1
  entries="$(find "$dir" -mindepth 1 -print -quit)" || return 1
  [[ -z "$entries" ]]
}
self_test() {
  local code dir
  for code in 1 2 7 78 123; do
    semantic_failure_exit "$code" || die "rejected valid compiler exit $code"
  done
  for code in 0 124 125 126 127 128 137 143 255 -1 a '' 01; do
    if semantic_failure_exit "$code"; then die "accepted invalid exit '$code'"; fi
  done
  local timed_out=0
  timeout --signal=TERM --kill-after=1s 0.1s sleep 3 >/dev/null 2>&1 || timed_out=$?
  [[ "$timed_out" == 124 ]] || die "GNU timeout contract changed: status=$timed_out"
  dir="$(mktemp -d "${TMPDIR:-/tmp}/atto-aot-output-mutants.XXXXXXXX")"
  no_artifacts "$dir" || die 'empty output directory falsely rejected'
  find() { return 7; }
  if no_artifacts "$dir"; then die 'failing find falsely accepted'; fi
  unset -f find
  touch "$dir/guest.o"
  if no_artifacts "$dir"; then die 'object-file sidecar falsely accepted'; fi
  rm "$dir/guest.o"
  ln -s "$dir/nonexistent" "$dir/guest"
  if no_artifacts "$dir"; then die 'dangling executable symlink falsely accepted'; fi
  rm "$dir/guest"
  mkdir "$dir/intermediate"
  if no_artifacts "$dir"; then die 'residual directory falsely accepted'; fi
  rmdir "$dir/intermediate"
  no_artifacts "$dir" || die 'empty output directory rejected after cleanup'
  rmdir "$dir"
  echo 'ATTO_NEGATIVE_AOT_SELFTEST_PASS|timeout=denied|sidecars=denied|symlinks=denied'
}
if [[ "${1:-}" == '--self-test' ]]; then
  [[ "$#" -eq 1 ]] || die 'unexpected self-test arguments'
  self_test
  exit 0
fi
[[ "$#" -eq 0 ]] || die 'unexpected arguments'
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
cd "$root"
: "${ATTO_NEGATIVE_EXPECTED_HEAD:?exact source head required}"
[[ "$ATTO_NEGATIVE_EXPECTED_HEAD" =~ ^[0-9a-f]{40}$ ]] || die 'bad source SHA'
[[ "$(git rev-parse HEAD)" == "$ATTO_NEGATIVE_EXPECTED_HEAD" ]] || die 'source HEAD changed'
for path in scripts/verify-atto-negative-aot-artifacts.sh scripts/compile-aot.sh; do
  [[ -f "$path" && ! -L "$path" ]] || die "missing/symlinked script $path"
  [[ "$(git rev-parse "HEAD:$path")" == "$(git hash-object --no-filters "$path")" ]] ||
    die "script differs from exact head: $path"
done
: "${ORESLANG_LLVMC:?real compiler binary required}"
: "${ORES_NO_GC_IR_AUDIT:?real IR auditor required}"
[[ -x "$ORESLANG_LLVMC" && -f "$ORESLANG_LLVMC" && ! -L "$ORESLANG_LLVMC" ]] ||
  die 'native compiler missing or symlinked'
[[ -x "$ORES_NO_GC_IR_AUDIT" && -f "$ORES_NO_GC_IR_AUDIT" && ! -L "$ORES_NO_GC_IR_AUDIT" ]] ||
  die 'native IR auditor missing or symlinked'
command -v timeout >/dev/null || die 'timeout tool missing'
command -v clang >/dev/null || die 'clang missing'
scratch="$(mktemp -d "${RUNNER_TEMP:-${TMPDIR:-/tmp}}/atto-aot-denied.XXXXXXXX")"
trap 'rm -rf -- "$scratch"' EXIT
mkdir "$scratch/logs"
cases=(
  removed-modulo removed-caret removed-double-caret
  type-first-reassignment typed-const-reassignment
  legacy-async-modifier critical-suspends-modifier suspends-critical-modifier
)
for id in "${cases[@]}"; do
  case "$id" in
    removed-*|*-reassignment) source="contracts/atto-scalar-v1/negative/$id.atto" ;;
    *) source="tests/formal-policy-atto/negative/$id.atto" ;;
  esac
  [[ -f "$source" && ! -L "$source" ]] || die "missing/symlinked source $id"
  blob="$(git rev-parse "HEAD:$source")"
  [[ "$blob" == "$(git hash-object --no-filters "$source")" ]] || die "fixture drift $id"
  case_dir="$scratch/$id"
  mkdir "$case_dir"
  target="$case_dir/guest"
  status=0
  timeout --signal=TERM --kill-after=5s 30s \
    bash "$root/scripts/compile-aot.sh" "$root/$source" "$target" \
    > "$scratch/logs/$id.stdout" 2> "$scratch/logs/$id.stderr" || status=$?
  semantic_failure_exit "$status" ||
    die "AOT source accepted, timed out, crashed or tool unavailable: $id status=$status"
  [[ -s "$scratch/logs/$id.stderr" ]] || die "missing compiler diagnostic: $id"
  no_artifacts "$case_dir" && [[ ! -e "$target" && ! -L "$target" ]] ||
    die "AOT build leaked executable, object, symlink or directory: $id"
  [[ "$blob" == "$(git hash-object --no-filters "$source")" ]] ||
    die "compiler mutated canonical source: $id"
  printf 'ATTO_AOT_NEGATIVE_NO_ARTIFACT|case=%s|blob=%s|status=%s\n' "$id" "$blob" "$status"
done
echo "ATTO_AOT_NEGATIVE_ALL_PASS|cases=8|head=$ATTO_NEGATIVE_EXPECTED_HEAD|scope=artifact"
