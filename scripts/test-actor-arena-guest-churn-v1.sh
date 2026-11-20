#!/usr/bin/env bash
# Compiler-backed .atto actor churn; C host tests cannot substitute for this.
# Successful output is execution evidence, NOT allocator/FFI/zero-GC certification.
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
: "${EXPECTED_HEAD:?provide immutable exact head}"
: "${ORESLANG_LLVMC:?provide exact-head oreslang-llvmc}"
: "${ORES_NO_GC_IR_AUDIT:?provide compiled Rust IR auditor}"
: "${CLANG:?provide Clang}"
[[ "$EXPECTED_HEAD" =~ ^[0-9a-f]{40}$ ]] || exit 64
[[ "$(git -C "$root" rev-parse HEAD)" == "$EXPECTED_HEAD" ]] || exit 78
[[ -z "$(git -C "$root" status --porcelain)" ]] || exit 78
for tool in git sha256sum cmp timeout readelf; do command -v "$tool" >/dev/null || exit 69; done
compiler="$(realpath -e -- "$ORESLANG_LLVMC")"
auditor="$(realpath -e -- "$ORES_NO_GC_IR_AUDIT")"
[[ -x "$compiler" && -x "$auditor" && "$(basename "$compiler")" == 'oreslang-llvmc' ]] || exit 69
command -v -- "$CLANG" >/dev/null || exit 69
source_rel="tests/fixtures/actor-arena-lifecycle-v1/churn-128.atto"
negative_rel="tests/fixtures/actor-state-e2e-v1/sealed-direct-state-reject.atto"
for rel in "$source_rel" "$negative_rel"; do
  [[ "$rel" == *.atto && -f "$root/$rel" && ! -L "$root/$rel" ]] || exit 78
  [[ "$(git -C "$root" ls-files --stage -- "$rel")" =~ ^100644[[:space:]] ]] || exit 78
  test "$(git -C "$root" rev-parse "HEAD:$rel")" = "$(git -C "$root" hash-object --no-filters "$root/$rel")" || exit 78
done
source="$root/$source_rel"
negative="$root/$negative_rel"
work="$(mktemp -d)"; trap 'rm -rf -- "$work"' EXIT
printf 'LLVM_ACTOR_HEAP_GUEST_CHURN_OK|128\n' > "$work/expected"
for opt in 0 2; do
  export ORESLANG_LLVM_AOT_OPT_LEVEL="$opt"
  if ! timeout 120s "$compiler" --target=host "$source" >"$work/churn-$opt.ll" 2>"$work/compiler-$opt.err"; then
    echo "LLVM_ACTOR_HEAP_COMPILER_NOT_ADMITTED|O$opt|guest=NOT_EXECUTED" >&2
    cat "$work/compiler-$opt.err" >&2
    exit 1
  fi
  [[ -s "$work/churn-$opt.ll" ]] || { echo 'compiler emitted no IR' >&2; exit 1; }
  # A constant-output native binary is not evidence that actors executed.
  # Require an actual call or invoke to a source-actor runtime ABI; mere
  # declarations, strings and LLVM IR comments must never satisfy this gate.
  if ! awk '
    {
      line=$0
      # Generated ABI symbol names are never quoted strings containing
      # semicolons. Ignore trailing inline comments used to forge "calls".
      sub(/;.*/, "", line)
      if (line ~ /[[:space:]](call|invoke)[[:space:]]/ &&
          line ~ /@"?(oreslang_source_actor_|ores_actor_)[A-Za-z0-9_.$]*[[:space:]]*\(/)
        actor_call=1
    }
    END { exit(actor_call ? 0 : 1) }
  ' "$work/churn-$opt.ll"; then
    echo "LLVM_ACTOR_HEAP_GUEST_NOT_PROVEN|O$opt|no_source_actor_runtime_calls" >&2
    exit 1
  fi
  "$auditor" "$work/churn-$opt.ll" >"$work/no-gc-$opt.log"
  artifact="$work/native-$opt"
  timeout 120s "$root/scripts/compile-aot.sh" "$source" "$artifact" >"$work/build-$opt.log"
  [[ -f "$artifact" && -x "$artifact" && ! -L "$artifact" ]] || exit 1
  readelf -Wh "$artifact" >/dev/null || exit 1
  for ((run=1;run<=4;run++)); do
    rc=0
    timeout --signal=TERM 20s "$artifact" >"$work/out" 2>"$work/err" || rc=$?
    [[ "$rc" == 0 ]] && cmp -s "$work/out" "$work/expected" && [[ ! -s "$work/err" ]] || {
      echo "LLVM_ACTOR_HEAP_NATIVE_FAILED|O$opt|run=$run|exit=$rc" >&2
      cat "$work/out" "$work/err" >&2
      exit 1
    }
  done
  rc=0
  timeout 30s "$compiler" --target=host "$negative" >"$work/reject-$opt.ll" 2>"$work/reject-$opt.err" || rc=$?
  [[ "$rc" != 0 && "$rc" != 124 && "$rc" != 125 && ! -s "$work/reject-$opt.ll" ]] || {
    echo 'sealed actor direct state was admitted or emitted executable IR' >&2; exit 1;
  }
  grep -Eiq 'sealed.*(actor|state|field)|actor.state.*(denied|private|sealed|access)|handle_state.*(required|only|must)|critical-(read|write).*(required|only|must)' "$work/reject-$opt.err" || {
    cat "$work/reject-$opt.err" >&2; exit 1;
  }
  printf 'LLVM_ACTOR_HEAP_GUEST_CANDIDATE_EXECUTED_NOT_CERTIFIED|head=%s|O%s|incarnations=128|runs=4|positive_blob=%s|negative_blob=%s|compiler_sha256=%s|native_sha256=%s\n' \
    "$EXPECTED_HEAD" "$opt" "$(git -C "$root" rev-parse "HEAD:$source_rel")" \
    "$(git -C "$root" rev-parse "HEAD:$negative_rel")" \
    "$(sha256sum "$compiler" | cut -d' ' -f1)" "$(sha256sum "$artifact" | cut -d' ' -f1)"
done
unset ORESLANG_LLVM_AOT_OPT_LEVEL
[[ "$(git -C "$root" rev-parse HEAD)" == "$EXPECTED_HEAD" ]] || exit 78
echo "LLVM_ACTOR_HEAP_GUEST_MATRIX_EXECUTED|head=$EXPECTED_HEAD|levels=O0_O2|process_executions=8|negative_ir_denials=2|runtime_heap_proof=NOT_CERTIFIED"
