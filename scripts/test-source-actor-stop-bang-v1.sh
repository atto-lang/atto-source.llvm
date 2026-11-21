#!/usr/bin/env bash
# Pin actual compiler/guest evidence for the native stop!/stop alias.
# No source-free model, synthetic stdout, fake IR or status-only PASS.
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
: "${EXPECTED_HEAD:?immutable exact source commit SHA is required}"
: "${ORESLANG_LLVMC:?native compiler executable is required}"
: "${EXPECTED_COMPILER_SHA256:?independently pinned exact-head compiler sha256 is required}"
[[ "$EXPECTED_HEAD" =~ ^[a-f0-9]{40}$ ]] || exit 64
[[ "$EXPECTED_COMPILER_SHA256" =~ ^[a-f0-9]{64}$ ]] || exit 64
for executable in git timeout grep cmp sha256sum realpath; do
  command -v "$executable" >/dev/null || exit 69
done
root_head="$(git -C "$root" rev-parse HEAD)"
[[ "$root_head" == "$EXPECTED_HEAD" ]] || { echo SOURCE_HEAD_MISMATCH >&2; exit 78; }
[[ -z "$(git -C "$root" status --porcelain --untracked-files=all)" ]] || exit 78
inputs=(
  tests/fixtures/source-actor-spawn-stop.ores
  tests/fixtures/source-actor-spawn-stop-bang.ores
  tests/fixtures/source-actor-send-after-stop-bang.ores
  tests/fixtures/source-actor-stop-bang-no-boundary.ores
  tests/fixtures/source-actor-stop-bang-no-adjacency.ores
  tests/fixtures/source-actor-stop-double-bang.ores
  tests/fixtures/source-actor-pipe-bang-unlowered-reject.ores
  tests/fixtures/source-actor-rpipe-bang-unlowered-reject.ores
)
for f in "${inputs[@]}"; do
  [[ -f "$root/$f" && ! -L "$root/$f" ]] || exit 78
  [[ "$(git -C "$root" hash-object --no-filters "$root/$f")" == \
     "$(git -C "$root" rev-parse "HEAD:$f")" ]] || exit 78
done
compiler="$(realpath -e "$ORESLANG_LLVMC")"
[[ -x "$compiler" ]] || exit 69
compiler_sha256="$(sha256sum "$compiler" | cut -d " " -f1)"
[[ "$compiler_sha256" == "$EXPECTED_COMPILER_SHA256" ]] || { echo COMPILER_BINARY_SHA_MISMATCH >&2; exit 78; }
work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT
for opt in 0 2; do
  export ORESLANG_LLVM_AOT_OPT_LEVEL="$opt"
  for variant in bare bang; do
    if [[ "$variant" == bare ]]; then
      input=tests/fixtures/source-actor-spawn-stop.ores
    else
      input=tests/fixtures/source-actor-spawn-stop-bang.ores
    fi
    timeout 120s "$compiler" --target=host "$root/$input" >"$work/program.ll" 2>"$work/compiler.err"
    [[ -s "$work/program.ll" && ! -s "$work/compiler.err" ]] || exit 1
    grep -Eq 'call[[:space:]].*@oreslang_source_actor_stop_id[(]' "$work/program.ll" ||
      { echo STOP_BANG_NOT_LOWERED_TO_CHECKED_STOP >&2; exit 1; }
    if grep -Eq 'call[[:space:]].*@oreslang_source_actor_stop[(]' "$work/program.ll"; then
      echo LEGACY_POINTER_STOP_NOT_ALLOWED >&2; exit 1
    fi
    bin="$work/stop-$variant-$opt"
    timeout 120s "$root/scripts/compile-aot.sh" "$root/$input" "$bin" >"$work/build.log"
    [[ -f "$bin" && -x "$bin" && ! -L "$bin" ]] || exit 1
    for run in 1 2 3; do
      result=0
      timeout 45s "$bin" >"$work/out" 2>"$work/err" || result=$?
      if [[ "$result" != 43 || -s "$work/out" || -s "$work/err" ]]; then
        echo "STOP_BANG_GUEST_FAIL|$variant|O$opt|run=$run|exit=$result" >&2
        cat "$work/out" "$work/err" >&2
        exit 1
      fi
    done
    echo "STOP_BANG_GUEST_EXECUTED|$variant|O$opt|head=$EXPECTED_HEAD|guest_runs=3|exit=43|sha256=$(sha256sum "$bin" | cut -d' ' -f1)"
  done
  for negative in \
    source-actor-send-after-stop-bang.ores \
    source-actor-stop-bang-no-boundary.ores \
    source-actor-stop-bang-no-adjacency.ores \
    source-actor-stop-double-bang.ores \
    source-actor-pipe-bang-unlowered-reject.ores \
    source-actor-rpipe-bang-unlowered-reject.ores; do
    result=0
    timeout 60s "$compiler" --target=host "$root/tests/fixtures/$negative" \
       >"$work/rejected.ll" 2>"$work/rejected.err" || result=$?
    if [[ "$result" == 0 || "$result" == 124 || -s "$work/rejected.ll" || ! -s "$work/rejected.err" ]]; then
      echo "STOP_BANG_INVALID_ACCEPTED|$negative|O$opt|exit=$result" >&2
      exit 1
    fi
  done
done
[[ "$(git -C "$root" rev-parse HEAD)" == "$EXPECTED_HEAD" ]] || exit 78
echo "STOP_BANG_EXACT_HEAD_MATRIX_EXECUTED|head=$EXPECTED_HEAD|compiler_sha256=$compiler_sha256|native_runs=12|negative_rejections=12"
