#!/usr/bin/env bash
# Real LLVM source actor guest: 128 sequential spawn + synchronous physical
# stop/join iterations. This does NOT certify async await/get_done or zero GC.
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
: "${EXPECTED_HEAD:?provide immutable exact-head SHA}"
: "${ORESLANG_LLVMC:?build the exact-head LLVM compiler}"
: "${ORES_NO_GC_IR_AUDIT:?build the exact-head Rust IR auditor}"
: "${CLANG:?provide Clang}"
[[ "$EXPECTED_HEAD" =~ ^[0-9a-f]{40}$ ]] || exit 64
for tool in git timeout cmp readelf sha256sum grep awk; do
  command -v "$tool" >/dev/null || { echo "missing $tool" >&2; exit 69; }
done
source_rel="tests/fixtures/source-actor-bounded-churn-v1/positive.atto"
more_positive_rels=(
  "tests/fixtures/source-actor-bounded-churn-v1/positive-do-done.atto"
  "tests/fixtures/source-actor-bounded-churn-v1/positive-zero.atto"
)
negative_rels=(
  "tests/fixtures/source-actor-bounded-churn-v1/missing-stop.atto"
  "tests/fixtures/source-actor-bounded-churn-v1/double-stop.atto"
  "tests/fixtures/source-actor-bounded-churn-v1/unbounded-count.atto"
  "tests/fixtures/source-actor-bounded-churn-v1/await-suspension.atto"
  "tests/fixtures/source-actor-bounded-churn-v1/foreign-stop.atto"
)
check_snapshot() {
  [[ "$(git -C "$root" rev-parse HEAD)" == "$EXPECTED_HEAD" ]] || return 1
  [[ -z "$(git -C "$root" status --porcelain --untracked-files=all)" ]] || return 1
  for rel in "$source_rel" "${more_positive_rels[@]}" "${negative_rels[@]}"; do
    [[ -f "$root/$rel" && ! -L "$root/$rel" ]] || return 1
    [[ "$(git -C "$root" ls-files --stage -- "$rel")" =~ ^100644[[:space:]] ]] || return 1
    test "$(git -C "$root" rev-parse "HEAD:$rel")" = "$(git -C "$root" hash-object --no-filters "$root/$rel")" || return 1
  done
}
check_snapshot || { echo 'SOURCE_ACTOR_CHURN_EXACT_HEAD_MISMATCH' >&2; exit 78; }
compiler="$(realpath -e "$ORESLANG_LLVMC")"
auditor="$(realpath -e "$ORES_NO_GC_IR_AUDIT")"
[[ -x "$compiler" && "$(basename "$compiler")" == 'oreslang-llvmc' && -x "$auditor" ]] || exit 69
command -v -- "$CLANG" >/dev/null || exit 69
work="$(mktemp -d)"; trap 'rm -rf -- "$work"' EXIT
for opt in 0 2; do
  export ORESLANG_LLVM_AOT_OPT_LEVEL="$opt"
  timeout 120s "$compiler" --target=host "$root/$source_rel" >"$work/source-$opt.ll" 2>"$work/compiler-$opt.err" || {
    cat "$work/compiler-$opt.err" >&2
    echo "SOURCE_ACTOR_CHURN_GUEST_NOT_COMPILED|O$opt" >&2
    exit 1
  }
  [[ -s "$work/source-$opt.ll" && ! -s "$work/compiler-$opt.err" ]] || exit 1
  "$auditor" "$work/source-$opt.ll" >"$work/gc-audit-$opt.log"
  # IR must contain actual call sites, not declarations, for both effects.
  # Named CFG blocks prove the runtime calls occur in the loop body, not in
  # constructor/handler definitions or an unrelated static test function.
  grep -Eq '^actor\.churn\.condition:' "$work/source-$opt.ll" || exit 1
  grep -Eq '^actor\.churn\.scoped\.body:' "$work/source-$opt.ll" || exit 1
  awk '
    /^actor[.]churn[.]scoped[.]body:/ { body=1; next }
    body && /^[[:alnum:]_.]+:/ { exit }
    body && /[[:space:]]call[[:space:]]/ && /@oreslang_source_actor_spawn[[:space:]]*[(]/ {
      if (stage != 0) bad=1; stage=1
    }
    body && /[[:space:]]call[[:space:]]/ &&
      /@oreslang_source_actor_wait_ready_blocking[[:space:]]*[(]/ {
      if (stage != 1) bad=1; stage=2
    }
    body && /[[:space:]]call[[:space:]]/ && /@oreslang_source_actor_stop[[:space:]]*[(]/ {
      if (stage != 2) bad=1; stage=3
    }
    END { exit (stage == 3 && !bad ? 0 : 1) }
  ' "$work/source-$opt.ll" || {
    echo "SOURCE_ACTOR_CHURN_MISSING_ORDERED_READY_BARRIER|O$opt" >&2; exit 1;
  }
  artifact="$work/source-actor-guest-$opt"
  timeout 120s "$root/scripts/compile-aot.sh" "$root/$source_rel" "$artifact" >"$work/build-$opt.log"
  [[ -x "$artifact" && -f "$artifact" && ! -L "$artifact" ]] || exit 1
  readelf -Wh "$artifact" >/dev/null
  for run in 1 2 3 4; do
    rc=0
    timeout 45s "$artifact" >"$work/out" 2>"$work/err" || rc=$?
    if [[ "$rc" != 128 || -s "$work/out" || -s "$work/err" ]]; then
      echo "SOURCE_ACTOR_CHURN_EXECUTION_FAIL|O$opt|run=$run|exit=$rc" >&2
      cat "$work/out" "$work/err" >&2
      exit 1
    fi
  done
  # Additional accepted forms: canonical while/do/done and zero-trip
  # actor churn. The latter must exit without constructing any actor.
  expected_statuses=(7 0)
  for index in 0 1; do
    rel="${more_positive_rels[index]}"
    desired="${expected_statuses[index]}"
    timeout 120s "$compiler" --target=host "$root/$rel" >"$work/variant.ll" 2>"$work/variant.err" || {
      cat "$work/variant.err" >&2
      echo "SOURCE_ACTOR_CHURN_VARIANT_NOT_COMPILED|O$opt|file=$rel" >&2
      exit 1
    }
    [[ -s "$work/variant.ll" && ! -s "$work/variant.err" ]] || exit 1
    "$auditor" "$work/variant.ll" >"$work/variant-audit.log"
    grep -Eq '^actor\.churn\.scoped\.body:' "$work/variant.ll" || exit 1
    variant_native="$work/variant-$opt-$index"
    timeout 120s "$root/scripts/compile-aot.sh" "$root/$rel" "$variant_native" >/dev/null
    [[ -x "$variant_native" && ! -L "$variant_native" ]] || exit 1
    rc=0
    timeout 45s "$variant_native" >"$work/variant.out" 2>"$work/variant.stderr" || rc=$?
    if [[ "$rc" != "$desired" || -s "$work/variant.out" || -s "$work/variant.stderr" ]]; then
      echo "SOURCE_ACTOR_CHURN_VARIANT_FAIL|O$opt|file=$rel|exit=$rc|expected=$desired" >&2
      cat "$work/variant.out" "$work/variant.stderr" >&2
      exit 1
    fi
  done
  for rel in "${negative_rels[@]}"; do
    negative="$root/$rel"
    rc=0
    timeout 30s "$compiler" --target=host "$negative" >"$work/negative.ll" 2>"$work/negative.err" || rc=$?
    case "${rel##*/}" in
      missing-stop.atto|await-suspension.atto)
        expected_error="expected.*stop" ;;
      double-stop.atto)
        expected_error="(expected.*identifier|bounded actor churn.*increment)" ;;
      unbounded-count.atto)
        expected_error="bounded actor churn count" ;;
      foreign-stop.atto)
        expected_error="bounded actor churn must stop its exact scoped actor" ;;
      *) echo "unreviewed negative fixture: $rel" >&2; exit 78 ;;
    esac
    if [[ "$rc" == 0 || "$rc" == 124 || "$rc" == 125 || -s "$work/negative.ll" ]] ||
      ! grep -Eiq "$expected_error" "$work/negative.err"; then
      echo "SOURCE_ACTOR_CHURN_INVALID_ACCEPTED|O$opt|file=$rel|exit=$rc" >&2
      cat "$work/negative.err" >&2
      exit 1
    fi
  done
  check_snapshot || { echo 'SOURCE_ACTOR_CHURN_SOURCE_DRIFT' >&2; exit 78; }
  printf 'SOURCE_ACTOR_BOUNDED_CHURN_GUEST_EXECUTED|head=%s|O%s|actors=128|runs=4|negative=5|source_blob=%s|compiler_sha256=%s|native_sha256=%s|gc=IR_SCREEN_ONLY\n' \
    "$EXPECTED_HEAD" "$opt" "$(git -C "$root" rev-parse "HEAD:$source_rel")" \
    "$(sha256sum "$compiler" | cut -d' ' -f1)" \
    "$(sha256sum "$artifact" | cut -d' ' -f1)"
done
echo "SOURCE_ACTOR_BOUNDED_CHURN_MATRIX_EXECUTED|head=$EXPECTED_HEAD|levels=O0_O2|native_executions=12|positive_variants=3|negative_compiler_rejections=10|async_guest=NOT_EXECUTED|zero_gc_certified=NO"
