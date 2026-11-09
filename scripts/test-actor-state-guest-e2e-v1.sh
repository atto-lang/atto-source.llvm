#!/usr/bin/env bash
# Real LLVM native guest candidate; never standalone release certification.
set -euo pipefail
root="$(cd -- "$(dirname -- "$0")/.." && pwd -P)"
: "$EXPECTED_HEAD"
[[ "$EXPECTED_HEAD" =~ ^[0-9a-f]{40}$ ]] || exit 64
[[ "$(git -C "$root" rev-parse HEAD)" == "$EXPECTED_HEAD" ]] || exit 78
for tool in timeout clang sha256sum git; do command -v "$tool" >/dev/null || exit 69; done
for name in positive sealed-direct-state-reject; do
  rel="tests/fixtures/actor-state-e2e-v1/$name.atto"
  src="$root/$rel"
  [[ -f "$src" && ! -L "$src" ]] || exit 78
  [[ "$(git -C "$root" ls-files --stage -- "$rel")" =~ ^100644[[:space:]] ]] || exit 78
  [[ "$(git -C "$root" rev-parse "HEAD:$rel")" == "$(git -C "$root" hash-object --no-filters "$src")" ]] || exit 78
done
compiler="$root/build/oreslang-llvmc"
auditor="$root/build/oreslang-no-gc-ir-audit"
[[ -x "$compiler" && -x "$auditor" && ! -L "$compiler" && ! -L "$auditor" ]] || {
  echo "build exact-head compiler and IR auditor first" >&2; exit 69;
}
tmp="$(mktemp -d)"
trap 'rm -rf -- "$tmp"' EXIT
positive="$root/tests/fixtures/actor-state-e2e-v1/positive.atto"
negative="$root/tests/fixtures/actor-state-e2e-v1/sealed-direct-state-reject.atto"
printf 'ACTOR_E2E_OK|42\n' > "$tmp/expected"
for level in 0 2; do
  export ORESLANG_LLVM_AOT_OPT_LEVEL="$level"
  timeout 90s "$compiler" --target=host "$positive" > "$tmp/positive-$level.ll"
  "$auditor" "$tmp/positive-$level.ll" > "$tmp/audit-$level"
  artifact="$tmp/guest-$level"
  timeout 90s "$root/scripts/compile-aot.sh" "$positive" "$artifact" > "$tmp/build-$level"
  [[ -x "$artifact" && -f "$artifact" && ! -L "$artifact" ]] || exit 1
  for ((i=1;i<=16;i++)); do
    rc=0
    timeout 15s "$artifact" > "$tmp/out" 2> "$tmp/err" || rc=$?
    [[ "$rc" == 0 ]] && cmp -s "$tmp/out" "$tmp/expected" && [[ ! -s "$tmp/err" ]] || {
      echo "LLVM O$level guest failed iteration=$i exit=$rc" >&2
      cat "$tmp/out" "$tmp/err" >&2; exit 1;
    }
  done
  printf 'LLVM_ACTOR_GUEST_EXECUTED_NOT_CERTIFIED|head=%s|O%s|repeats=16|source_sha256=%s|compiler_sha256=%s|guest_sha256=%s\n' \
    "$EXPECTED_HEAD" "$level" "$(sha256sum "$positive" | cut -d' ' -f1)" \
    "$(sha256sum "$compiler" | cut -d' ' -f1)" "$(sha256sum "$artifact" | cut -d' ' -f1)"
done
unset ORESLANG_LLVM_AOT_OPT_LEVEL
rc=0
timeout 30s "$compiler" --target=host "$negative" > "$tmp/negative.ll" 2> "$tmp/negative.err" || rc=$?
[[ "$rc" != 0 && "$rc" != 124 && "$rc" != 125 ]] || exit 1
[[ ! -s "$tmp/negative.ll" ]] || { echo "compiler emitted IR for rejected sealed-state program" >&2; exit 1; }
grep -Eiq 'sealed.*(actor|state|field)|actor.state.*(denied|private|sealed|access)|handle_state.*(required|only|must)|critical-(read|write).*(required|only|must)' "$tmp/negative.err" || {
  cat "$tmp/negative.err" >&2; exit 1;
}
[[ "$(git -C "$root" rev-parse HEAD)" == "$EXPECTED_HEAD" ]] || exit 78
for name in positive sealed-direct-state-reject; do
  rel="tests/fixtures/actor-state-e2e-v1/$name.atto"
  [[ "$(git -C "$root" rev-parse "HEAD:$rel")" == "$(git -C "$root" hash-object --no-filters "$root/$rel")" ]] || exit 78
done
