#!/usr/bin/env bash
# Native compiled Oreslang guest proof (LLVM frontend -> Clang executable).
set -euo pipefail
cd "$(dirname "$0")/.."
[[ "$(git rev-parse HEAD)" = "${EXPECTED_SOURCE_SHA:?exact head required}" ]]
[[ -x build/oreslang-llvmc ]] || { echo 'NATIVE_LLVM_COMPILER_NOT_AVAILABLE' >&2; exit 1; }
command -v clang >/dev/null
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
echo "SG_LLVM_BEGIN head=$(git rev-parse HEAD) tree=$(git rev-parse 'HEAD^{tree}') compiler_sha256=$(sha256sum build/oreslang-llvmc | cut -d' ' -f1)"
shared=contracts/stategraph-portability/shared-scalar.ores
[[ "$(git hash-object "$shared")" = "ec1cb5e6cc38c26842d724ccacb0df028b702c3f" ]] || {
  echo 'SG_LLVM_SHARED_SCALAR_SOURCE_MISMATCH' >&2; exit 1;
}
git ls-files --error-unmatch -- "$shared" >/dev/null
[[ -f "$shared" && ! -L "$shared" ]]
timeout --kill-after=3s 90s bash scripts/compile-aot.sh "$shared" "$tmp/common"
[[ -x "$tmp/common" ]]
set +e
timeout --kill-after=3s 30s "$tmp/common" > "$tmp/out" 2> "$tmp/err"
common_status=$?
set -e
if [[ "$common_status" -ne 42 || -s "$tmp/out" ]]; then
  cat "$tmp/err" >&2
  echo "SG_LLVM_COMMON_SCALAR_NOT_ACCEPTED status=$common_status" >&2
  exit 1
fi
printf 'SG_LLVM_COMMON_SCALAR_NATIVE_PASS blob=%s guest_exit=42\n' "$(git rev-parse "HEAD:$shared")"


# Safety boundary until backend generic object arrays + first-class callbacks
# have real lowering. Rejection (no partial IR) is NOT feature parity.
negative=tests/fixtures/stategraph-parity-generic-frontier-unsupported.ores
[[ -f "$negative" && ! -L "$negative" ]]
git ls-files --error-unmatch -- "$negative" >/dev/null
[[ "$(git hash-object "$negative")" = "$(git rev-parse "HEAD:$negative")" ]] ||
  { echo 'SG_LLVM_UNSUPPORTED_FRONTIER_SOURCE_DRIFT' >&2; exit 1; }
set +e
timeout --kill-after=3s 30s build/oreslang-llvmc --target=host "$negative" > "$tmp/negative.ll" 2> "$tmp/negative.err"
negative_status=$?
set -e
if [[ "$negative_status" -eq 0 || -s "$tmp/negative.ll" ]]; then
  echo "SG_LLVM_UNSUPPORTED_FRONTIER_NOT_FAIL_CLOSED status=$negative_status" >&2
  exit 1
fi
printf 'SG_LLVM_UNSUPPORTED_FRONTIER_REJECTED blob=%s (NOT parity)\n' \
  "$(git rev-parse "HEAD:$negative")"

# First byte-identical generic userland StateGraph source.
generic=contracts/stategraph-portability/shared-generic-state.ores
[[ -f "$generic" && ! -L "$generic" ]]
git ls-files --error-unmatch -- "$generic" >/dev/null
[[ "$(git hash-object "$generic")" = "c3a67b837a73f427a6157c446cc6e3989e9a66da" ]] ||
  { echo 'SG_LLVM_GENERIC_SOURCE_DRIFT' >&2; exit 1; }
[[ "$(git rev-parse "HEAD:$generic")" = "c3a67b837a73f427a6157c446cc6e3989e9a66da" ]]
timeout --kill-after=3s 90s bash scripts/compile-aot.sh "$generic" "$tmp/generic" ||
  { echo 'SG_LLVM_GENERIC_LOWERING_NOT_ACCEPTED' >&2; exit 1; }
[[ -x "$tmp/generic" ]]
set +e
timeout --kill-after=3s 30s "$tmp/generic" > "$tmp/out" 2> "$tmp/err"
generic_status=$?
set -e
if [[ "$generic_status" -ne 42 || -s "$tmp/out" ]]; then
  cat "$tmp/err" >&2
  echo "SG_LLVM_GENERIC_GUEST_NOT_ACCEPTED status=$generic_status" >&2
  exit 1
fi
printf 'SG_LLVM_GENERIC_NATIVE_GUEST_PASS blob=%s result=42\n' "$(git rev-parse "HEAD:$generic")"


# Canonical scalar-superstep semantics: the IDENTICAL Oreslang bytes are in
# StateGraph, Java/Graal and LLVM. Does not assert generic graph lowering.
superstep=contracts/stategraph-portability/shared-superstep-scalar.ores
expected_superstep_blob=054d4e9fe297c520513686feae076dc921f581ac
[[ -f "$superstep" && ! -L "$superstep" ]]
git ls-files --error-unmatch -- "$superstep" >/dev/null
[[ "$(git hash-object "$superstep")" = "$expected_superstep_blob" ]] ||
  { echo 'SG_LLVM_SHARED_SUPERSTEP_SOURCE_DRIFT' >&2; exit 1; }
[[ "$(git rev-parse "HEAD:$superstep")" = "$expected_superstep_blob" ]] ||
  { echo 'SG_LLVM_SHARED_SUPERSTEP_HEAD_DRIFT' >&2; exit 1; }
timeout --kill-after=3s 90s bash scripts/compile-aot.sh "$superstep" "$tmp/superstep" ||
  { echo 'SG_LLVM_SHARED_SUPERSTEP_COMPILE_NOT_ACCEPTED' >&2; exit 1; }
[[ -x "$tmp/superstep" ]]
set +e
timeout --kill-after=3s 30s "$tmp/superstep" > "$tmp/out" 2> "$tmp/err"
superstep_status=$?
set -e
if [[ "$superstep_status" -ne 42 || -s "$tmp/out" || -s "$tmp/err" ]]; then
  cat "$tmp/err" >&2
  echo "SG_LLVM_SHARED_SUPERSTEP_GUEST_NOT_ACCEPTED status=$superstep_status" >&2
  exit 1
fi
printf 'SG_LLVM_SHARED_SUPERSTEP_NATIVE_PASS blob=%s guest_exit=42\n' "$expected_superstep_blob"


# Byte-identical bounded recursion/conditional-route guest in Java and LLVM.
cycle=contracts/stategraph-portability/shared-bounded-cycle.ores
cycle_blob=3c69cdf00c4e1390898ef0594eba76c54c0e2d7f
[[ -f "$cycle" && ! -L "$cycle" ]]
git ls-files --error-unmatch -- "$cycle" >/dev/null
[[ "$(git hash-object "$cycle")" = "$cycle_blob" ]] ||
  { echo 'SG_LLVM_SHARED_CYCLE_SOURCE_DRIFT' >&2; exit 1; }
[[ "$(git rev-parse "HEAD:$cycle")" = "$cycle_blob" ]] ||
  { echo 'SG_LLVM_SHARED_CYCLE_HEAD_DRIFT' >&2; exit 1; }
timeout --kill-after=3s 90s bash scripts/compile-aot.sh "$cycle" "$tmp/cycle" ||
  { echo 'SG_LLVM_SHARED_CYCLE_COMPILE_NOT_ACCEPTED' >&2; exit 1; }
[[ -x "$tmp/cycle" ]]
set +e
timeout --kill-after=3s 30s "$tmp/cycle" > "$tmp/out" 2> "$tmp/err"
cycle_status=$?
set -e
if [[ "$cycle_status" -ne 42 || -s "$tmp/out" || -s "$tmp/err" ]]; then
  cat "$tmp/err" >&2
  echo "SG_LLVM_SHARED_CYCLE_GUEST_NOT_ACCEPTED status=$cycle_status" >&2
  exit 1
fi
printf 'SG_LLVM_SHARED_CYCLE_NATIVE_PASS blob=%s guest_exit=42\n' "$cycle_blob"


# Actual LLVM compiler regression for canonical "if expr then ... fi".
# The same source blob is present in the Java/JVM parity branch.
canonical=contracts/stategraph-portability/shared-canonical-then.ores
canonical_blob=ad42a13d1542d8df6215bff42d056a489015c842
[[ -f "$canonical" && ! -L "$canonical" ]]
git ls-files --error-unmatch -- "$canonical" >/dev/null
[[ "$(git hash-object "$canonical")" = "$canonical_blob" ]]
[[ "$(git rev-parse "HEAD:$canonical")" = "$canonical_blob" ]]
timeout --kill-after=3s 90s bash scripts/compile-aot.sh "$canonical" "$tmp/canonical" ||
  { echo 'SG_LLVM_CANONICAL_THEN_COMPILER_NOT_ACCEPTED' >&2; exit 1; }
[[ -x "$tmp/canonical" ]]
set +e
timeout --kill-after=3s 30s "$tmp/canonical" > "$tmp/out" 2> "$tmp/err"
canonical_status=$?
set -e
if [[ "$canonical_status" -ne 42 || -s "$tmp/out" || -s "$tmp/err" ]]; then
  cat "$tmp/err" >&2
  echo "SG_LLVM_CANONICAL_THEN_GUEST_NOT_ACCEPTED status=$canonical_status" >&2
  exit 1
fi
printf 'SG_LLVM_CANONICAL_THEN_NATIVE_PASS blob=%s guest_exit=42\n' "$canonical_blob"

# Negative companion: "if x = y then" is assignment and must not parse.
bad_condition=tests/fixtures/stategraph-canonical-then-assignment-reject.ores
[[ -f "$bad_condition" && ! -L "$bad_condition" ]]
git ls-files --error-unmatch -- "$bad_condition" >/dev/null
[[ "$(git hash-object "$bad_condition")" = "$(git rev-parse "HEAD:$bad_condition")" ]]
set +e
timeout --kill-after=3s 30s build/oreslang-llvmc --target=host "$bad_condition" \
  > "$tmp/bad-condition.ll" 2> "$tmp/bad-condition.err"
bad_condition_status=$?
set -e
if [[ "$bad_condition_status" -eq 0 || -s "$tmp/bad-condition.ll" ]]; then
  echo "SG_LLVM_CANONICAL_CONDITION_NOT_FAIL_CLOSED status=$bad_condition_status" >&2
  exit 1
fi
printf 'SG_LLVM_CANONICAL_ASSIGNMENT_REJECTED blob=%s (negative proof only)\n' \
  "$(git rev-parse "HEAD:$bad_condition")"

for name in scalar guard nominal; do
  source="tests/fixtures/stategraph-parity-$name.ores"
  git ls-files --error-unmatch "$source" >/dev/null
  [[ -s "$source" && ! -L "$source" ]]
  timeout --kill-after=3s 90s bash scripts/compile-aot.sh "$source" "$tmp/$name" ||
    { echo "SG_LLVM_COMPILE_NOT_ACCEPTED $source" >&2; exit 1; }
  [[ -x "$tmp/$name" ]]
  set +e
  timeout --kill-after=3s 30s "$tmp/$name" > "$tmp/out" 2> "$tmp/err"
  status=$?
  set -e
  if [[ "$status" -ne 42 || -s "$tmp/out" ]]; then
    cat "$tmp/err" >&2
    echo "SG_LLVM_GUEST_NOT_ACCEPTED $name status=$status" >&2
    exit 1
  fi
  printf 'SG_LLVM_GUEST_PASS name=%s blob=%s guest_exit=42\n' \
    "$name" "$(git rev-parse "HEAD:$source")"
done
echo 'SG_LLVM_NATIVE_GUEST_PASS count=3 (NOT full StateGraph or actor parity)'
