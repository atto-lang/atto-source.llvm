#!/usr/bin/env bash
# Real LLVM O0/O2 native guest acceptance CANDIDATE. NOT_CERTIFIED without
# independent supervisor CONTROL and actor heap provenance receipts.
set -euo pipefail
root="$(cd -- "$(dirname -- "$0")/.." && pwd -P)"
: "${EXPECTED_HEAD:?supply exact PR head}"
[[ "$EXPECTED_HEAD" =~ ^[0-9a-f]{40}$ ]] || exit 64
[[ "$(git -C "$root" rev-parse HEAD)" == "$EXPECTED_HEAD" ]] || exit 78
for tool in timeout clang sha256sum git cmp; do command -v "$tool" >/dev/null || exit 69; done
relative="tests/fixtures/actor-two-peer-failure-v1/two-peer-failure-matrix.atto"
source="$root/$relative"
[[ -f "$source" && ! -L "$source" ]] || exit 78
[[ "$(git -C "$root" rev-parse "HEAD:$relative")" == eaac813262ca184930ba124180ed692e21329969 ]] || exit 78
[[ "$(git -C "$root" hash-object --no-filters "$source")" == eaac813262ca184930ba124180ed692e21329969 ]] || exit 78
[[ "$(git -C "$root" ls-files --stage -- "$relative")" =~ ^100644[[:space:]] ]] || exit 78
compiler="$root/build/oreslang-llvmc"
auditor="$root/build/oreslang-no-gc-ir-audit"
[[ -x "$compiler" && -x "$auditor" && ! -L "$compiler" && ! -L "$auditor" ]] || {
  echo 'Build the exact-head compiler and IR auditor first' >&2
  exit 69
}
tmp="$(mktemp -d)"
trap 'rm -rf -- "$tmp"' EXIT
printf 'ACTOR_TWO_PEER_V1_OK|6|42\n' > "$tmp/expected"
for level in 0 2; do
  export ORESLANG_LLVM_AOT_OPT_LEVEL="$level"
  if ! timeout 90s "$compiler" --target=host "$source" > "$tmp/guest-$level.ll" 2> "$tmp/compiler-$level.err"; then
    echo "LLVM_ACTOR_TWO_PEER_COMPILER_NOT_ADMITTED|O$level|guest=NOT_EXECUTED" >&2
    cat "$tmp/compiler-$level.err" >&2
    exit 1
  fi
  test -s "$tmp/guest-$level.ll"
  "$auditor" "$tmp/guest-$level.ll" > "$tmp/no-gc-$level.log"
  artifact="$tmp/guest-$level"
  timeout 90s "$root/scripts/compile-aot.sh" "$source" "$artifact" > "$tmp/build-$level.log"
  [[ -x "$artifact" && -f "$artifact" && ! -L "$artifact" ]] || exit 1
  for ((iteration=1;iteration<=16;iteration++)); do
    rc=0
    (ulimit -s 512; timeout 15s "$artifact") > "$tmp/out" 2> "$tmp/err" || rc=$?
    if [[ "$rc" != 0 ]] || ! cmp -s "$tmp/out" "$tmp/expected" || [[ -s "$tmp/err" ]]; then
      echo "LLVM_ACTOR_TWO_PEER_NATIVE_FAIL|O$level|iteration=$iteration|exit=$rc" >&2
      cat "$tmp/out" "$tmp/err" >&2
      exit 1
    fi
  done
  printf 'LLVM_TWO_PEER_GUEST_STDOUT_CANDIDATE|head=%s|O%s|low_machine_stack_kib=512|repeats=16|compiler_sha256=%s|guest_sha256=%s\n' \
    "$EXPECTED_HEAD" "$level" "$(sha256sum "$compiler" | cut -d' ' -f1)" \
    "$(sha256sum "$artifact" | cut -d' ' -f1)"
done
unset ORESLANG_LLVM_AOT_OPT_LEVEL
# Independent authenticated runtime event ledger is mandatory. Guest stdout
# cannot prove absent hooks, heap provenance or authenticated parent CONTROL.
: "${ACTOR_TWO_PEER_VERIFIED_LEDGER:?trusted runtime ledger required; stdout alone is not proof}"
[[ -s "$ACTOR_TWO_PEER_VERIFIED_LEDGER" && ! -L "$ACTOR_TWO_PEER_VERIFIED_LEDGER" ]] || exit 78
[[ "$(git -C "$root" rev-parse HEAD)" == "$EXPECTED_HEAD" ]] || exit 78
echo "LLVM_TWO_PEER_NATIVE_CANDIDATE_ONLY|head=$EXPECTED_HEAD|fixture_git_blob=eaac813262ca184930ba124180ed692e21329969"
