#!/usr/bin/env bash
# Source compiler gate. Actor grammar and ML execution are NOT claimed.
# Requires one real native executable scalar intersection plus denied unsupported
# hierarchy/model forms. A passing rejection is not actor parity.
set -Eeuo pipefail
[[ $# -eq 3 ]] || { echo "usage: $0 ORESLANG_LLVMC CLANG SOURCE_ROOT" >&2; exit 2; }
compiler="$1"; clang="$2"; root="$3"
[[ -x "$compiler" ]] || { echo "missing actual LLVM source compiler" >&2; exit 2; }
clang="$(command -v "$clang" || true)"
[[ -n "$clang" && -x "$clang" ]] || { echo "missing Clang" >&2; exit 2; }
tmp="$(mktemp -d)"
trap 'rm -rf -- "$tmp"' EXIT
# d(0.5*(w*x-target)^2)/dw = (w*x-target)*x.
# w=2,x=1,target=42,lr=1 yields a numeric update of 42.
# Verify the exact reviewed bytes before touching any compiler. A local
# fixture with the same result but different syntax is NOT source parity.
cp -- "$root/tests/fixtures/actor-ml-scalar-step.ores" "$tmp/linear_update.ores"
chmod 0400 -- "$tmp/linear_update.ores"
printf '%s\n' 'pub fnc main(): int { return 2 - 1 * (2 * 1 - 42) * 1; }' |
  cmp -s - "$tmp/linear_update.ores" || {
    echo "shared scalar guest fixture differs from reviewed byte sequence" >&2
    exit 1
  }
"$compiler" --target=host "$tmp/linear_update.ores" >"$tmp/linear_update.ll" 2>"$tmp/positive.err"
printf '%s\n' 'pub fnc main(): int { return 2 - 1 * (2 * 1 - 42) * 1; }' |
  cmp -s - "$tmp/linear_update.ores" || {
    echo "source snapshot changed during native LLVM compilation" >&2
    exit 1
  }
[[ -s "$tmp/linear_update.ll" ]] || { echo "LLVM compiler did not produce scalar IR" >&2; exit 1; }
"$clang" -O0 -x ir "$tmp/linear_update.ll" -o "$tmp/linear_update"
set +e
"$tmp/linear_update" >"$tmp/out" 2>"$tmp/run.err"
actual=$?
set -e
[[ $actual -eq 42 && ! -s "$tmp/out" && ! -s "$tmp/run.err" ]] || {
  echo "Incorrect native scalar gradient step: exit=$actual" >&2; exit 1;
}
# These are source admission negatives, not a claim that valid actor methods,
# class inheritance, optimizer state or autograd already lower on LLVM.
cases=(
  ordinary_class_as_parent
  duplicate_concrete_parent
  incompatible_actor_parent
  state_loan_escape
  model_graph
  actor_trait_mixed_state
  canonical_counter_target
)
for testcase in "${cases[@]}"; do
  case "$testcase" in
    ordinary_class_as_parent)
      source='define class Plain as end
define actor Bad extends Plain as end';;
    duplicate_concrete_parent)
      source='define actor A as end
define actor B as end
define actor Bad extends A, B as end';;
    incompatible_actor_parent)
      source='define isoactor A as end
define actor Bad extends A as end';;
    state_loan_escape)
      source='define actor Bad as
 state { int count = 1; }
 pub get(): int { return self.count; }
end';;
    model_graph)
      source='define model Linear as Dense(2,1) end';;
    actor_trait_mixed_state)
      source='define trait BadTrait as
 let int shared_mutable = 1;
end
define actor Bad implements BadTrait as end';;
    canonical_counter_target)
      command -v git >/dev/null || { echo "git source blob verifier unavailable" >&2; exit 2; }
      actual_blob="$(git hash-object --no-filters "$root/tests/fixtures/actor-counter-target.ores")"
      [[ "$actual_blob" == "c49780b4b42251db20ae9ac864eb8606bf7da922" ]] || {
        echo "Counter actor source differs from shared canonical Git blob" >&2
        exit 1
      }
      source="$(cat -- "$root/tests/fixtures/actor-counter-target.ores")";;
  esac
  printf '%s\n' "$source" >"$tmp/$testcase.ores"
  set +e
  "$compiler" --target=host "$tmp/$testcase.ores" >"$tmp/$testcase.ll" 2>"$tmp/$testcase.err"
  status=$?
  set -e
  [[ $status -ne 0 ]] || { echo "$testcase: unsupported source was admitted" >&2; exit 1; }
  [[ $status -lt 126 ]] || { echo "$testcase: compiler crashed or could not execute (exit=$status)" >&2; exit 1; }
  [[ ! -s "$tmp/$testcase.ll" ]] || { echo "$testcase: emitted LLVM IR on denied source" >&2; exit 1; }
  [[ -s "$tmp/$testcase.err" ]] || { echo "$testcase: missing explicit rejection diagnostic" >&2; exit 1; }
done
echo "LLVM_ACTOR_HIERARCHY_ML_SOURCE_GATE: scalar_42_pass; seven_source_denials_pass"
