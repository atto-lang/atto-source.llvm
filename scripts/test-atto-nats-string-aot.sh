#!/usr/bin/env bash
# Canonical application-owned AttoNATS String-array guest, native LLVM AOT.
# No Java/host routing model and no semantic source rewriting. The native
# compiler cannot yet execute top-level namespace headers, so only the two
# byte-exact first-line headers are removed in a temporary linked source.
set -euo pipefail
[[ $# == 3 ]] || { echo "usage: $0 llvmc clang no-gc-ir-auditor" >&2; exit 64; }
compiler="$1"
clang="$2"
auditor="$3"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
[[ -x "$compiler" && -x "$auditor" ]] || {
  echo "REAL_LLVM_COMPILER_OR_AUDITOR_MISSING" >&2; exit 69;
}
command -v "$clang" >/dev/null || { echo "CLANG_MISSING" >&2; exit 69; }
pattern="$root/tests/fixtures/atto-nats/pretokenized_subjects.atto"
guest="$root/tests/fixtures/atto-nats/pretokenized_nats_match.atto"
check() {
  local path="$1" sha="$2"
  [[ -f "$path" ]] || { echo "APP_SOURCE_MISSING" >&2; exit 65; }
  [[ "$(git -C "$root" hash-object "$path")" == "$sha" ]] || {
    echo "UNEXPECTED_CANONICAL_ATTO_BLOB" >&2; exit 65;
  }
  # The checkout must reference these exact immutable application bytes.
  [[ "$(git -C "$root" rev-parse "HEAD:${path#"$root/"}")" == "$sha" ]] || {
    echo "CANONICAL_GIT_TREE_BLOB_MISMATCH" >&2; exit 65;
  }
}
check "$pattern" "1363e61ae08a1f960157345c33010cc0ebfc7068"
check "$guest" "3923f6c2f7d72c652cec0b76c6bef3b04c37ee09"
[[ "$(head -n 1 "$pattern")" == "use namespace core;" ]] || exit 65
[[ "$(head -n 1 "$guest")" == "use namespace guest;" ]] || exit 65
dir="$(mktemp -d)"
trap 'rm -rf -- "$dir"' EXIT
{
  tail -n +2 "$pattern"
  printf '\n'
  tail -n +2 "$guest"
} > "$dir/atto-nats-match.atto"
echo "NAMESPACE_NORMALIZATION=ONLY_EXACT_FIRST_LINE_REMOVED_PER_FILE"
echo "NATIVE_ATTO_APP_PATTERN_BLOB=1363e61ae08a1f960157345c33010cc0ebfc7068"
echo "NATIVE_ATTO_APP_GUEST_BLOB=3923f6c2f7d72c652cec0b76c6bef3b04c37ee09"
"$compiler" --target=host "$dir/atto-nats-match.atto" > "$dir/matcher.ll" 2>"$dir/compiler.stderr" || {
  echo "NATIVE_ATTO_NATS_MATCHER_COMPILER_REJECTED" >&2
  head -n 20 "$dir/compiler.stderr" >&2
  exit 1
}
[[ -s "$dir/matcher.ll" && ! -s "$dir/compiler.stderr" ]] || {
  echo "INVALID_COMPILER_IR_OR_DIAGNOSTICS" >&2; exit 1;
}
grep -Eq '^define i32 @main\(' "$dir/matcher.ll"
grep -Eq 'call .*@ores_string_equal' "$dir/matcher.ll"
grep -Eq 'call .*@oreslang_int_array_get' "$dir/matcher.ll"
"$auditor" "$dir/matcher.ll" >/dev/null
echo "REAL_LLVM_IR_AND_NO_GC_AUDIT=PASS"
# Negative programs must not emit usable IR: prevent silently accepting a
# non-String token or fabricating an Array<String>-owning class from an int.
sed 's/published_parts.add("eu");/published_parts.add(42);/' \
  "$dir/atto-nats-match.atto" > "$dir/invalid-element.atto"
sed 's/new NatsSubjectTokens(exact_parts);/new NatsSubjectTokens(42);/' \
  "$dir/atto-nats-match.atto" > "$dir/invalid-owner.atto"
grep -Fq 'published_parts.add(42);' "$dir/invalid-element.atto"
grep -Fq 'new NatsSubjectTokens(42);' "$dir/invalid-owner.atto"
for case in invalid-element invalid-owner; do
  if "$compiler" --target=host "$dir/$case.atto" > "$dir/$case.ll" 2> "$dir/$case.stderr"; then
    echo "UNSOUND_NATIVE_ATTO_ACCEPTANCE_$case" >&2; exit 1
  fi
  [[ ! -s "$dir/$case.ll" && -s "$dir/$case.stderr" ]] || {
    echo "PARTIAL_IR_OR_NO_REJECTION_$case" >&2; exit 1
  }
  echo "NATIVE_ATTO_NATS_NEGATIVE_$case=PASS"
done
printf 'ATTO_NATS_TOKEN_MATCH_PASS\n' > "$dir/expected.stdout"
runtime="$root/src/main/cpp/oreslang/runtime"
for level in 0 2; do
  "$clang" "-O$level" -x ir "$dir/matcher.ll" -x c -std=c11 \
    -I "$root/src/main/cpp" \
    "$runtime/NativeStringRegion.c" \
    "$runtime/NativeIntArrayRegion.c" \
    "$runtime/NativeRecordRegion.c" -pthread -o "$dir/matcher-O$level"
  [[ -s "$dir/matcher-O$level" ]]
  # NativeStringRegion is NIO-only for stdout: the guest must write to a
  # FIFO/pipe, never a regular file. pipefail preserves its real exit status.
  "$dir/matcher-O$level" 2>"$dir/stderr-O$level" | cat >"$dir/stdout-O$level"
  [[ ! -s "$dir/stderr-O$level" ]]
  cmp "$dir/expected.stdout" "$dir/stdout-O$level"
  echo "NATIVE_ATTO_NATS_STRING_MATCHER_O${level}_EXECUTED=PASS"
done
echo "FULL_BROKER_ACTOR_STATE_NOT_TESTED=true"
echo "SPEEDUP_NOT_MEASURED=true"
