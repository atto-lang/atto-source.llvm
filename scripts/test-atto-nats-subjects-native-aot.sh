#!/usr/bin/env bash
# Compile exact AttoNATS subject parsing/validation code using native LLVM.
# This is a synchronous kernel, NOT a TCP NATS broker/actor integration.
set -euo pipefail
[[ $# == 3 ]] || { echo "usage: $0 llvmc clang no-gc-ir-auditor" >&2; exit 64; }
compiler="$1"; clang="$2"; audit="$3"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
[[ -x "$compiler" && -x "$audit" ]] || { echo "REAL_LLVM_COMPILER_OR_AUDITOR_MISSING" >&2; exit 69; }
command -v "$clang" >/dev/null || exit 69
core="$root/tests/fixtures/atto-nats/subjects.atto"
guest="$root/tests/fixtures/atto-nats/native_subject_full_guest.atto"
verify_blob() {
  [[ "$(git -C "$root" hash-object "$1")" == "$2" ]] || { echo "ATTO_NATS_BLOB_MISMATCH" >&2; exit 65; }
  [[ "$(git -C "$root" rev-parse "HEAD:${1#"$root/"}")" == "$2" ]] || exit 65
}
verify_blob "$core" "538a98c2ec79a4d859cd41ac6b85fde9f62e6869"
verify_blob "$guest" "97ea98b32d85d074b6b7066d534414c5a61c9335"
[[ "$(head -n 1 "$core")" == "use namespace core;" ]]
[[ "$(head -n 1 "$guest")" == "use namespace guest;" ]]
scratch="$(mktemp -d)"
trap 'rm -rf -- "$scratch"' EXIT
{
  tail -n +2 "$core"
  printf '\n'
  tail -n +2 "$guest"
} > "$scratch/subjects.atto"
echo "ORIGINAL_ATTO_NATS_CORE_BLOB=538a98c2ec79a4d859cd41ac6b85fde9f62e6869"
echo "LLVM_NAMESPACE_HEADER_NORMALIZATION=EXACT_FIRST_LINE_ONLY"
"$compiler" --target=host "$scratch/subjects.atto" > "$scratch/subjects.ll" 2>"$scratch/compile.stderr" || {
  head -n 20 "$scratch/compile.stderr" >&2
  echo "CANONICAL_ATTO_NATS_SUBJECTS_LLVM_COMPILE_FAIL" >&2
  exit 1
}
[[ -s "$scratch/subjects.ll" && ! -s "$scratch/compile.stderr" ]]
grep -Eq '^define i32 @main\(' "$scratch/subjects.ll"
grep -Fq '@ores_string_contains_literal' "$scratch/subjects.ll"
grep -Fq '@ores_string_split_literal' "$scratch/subjects.ll"
grep -Fq '@oreslang_int_array_get' "$scratch/subjects.ll"
"$audit" "$scratch/subjects.ll" > /dev/null
echo "ATTO_NATS_INGRESS_LLVM_IR_NO_GC_SCREEN_PASS"
# All negatives must fail in the real compiler with zero emitted LLVM IR.
cat > "$scratch/invalid-needle.atto" <<'ATTO'
pub fnc main():int{val String src="a.b";val parts=src.split_literal(42);return 0;}
ATTO
cat > "$scratch/invalid-receiver.atto" <<'ATTO'
pub fnc main():int{val int src=42;val parts=src.split_literal(".");return 0;}
ATTO
cat > "$scratch/array-handle-escape.atto" <<'ATTO'
pub fnc main():int{val String src="a.b";val parts=src.split_literal(".");val int handle=parts;return handle;}
ATTO
for kind in invalid-needle invalid-receiver array-handle-escape; do
  if "$compiler" --target=host "$scratch/$kind.atto" >"$scratch/$kind.ll" 2>"$scratch/$kind.stderr"; then
    echo "UNSAFE_NATIVE_STRING_SPLIT_ADMITTED_$kind" >&2; exit 1
  fi
  [[ ! -s "$scratch/$kind.ll" && -s "$scratch/$kind.stderr" ]] || exit 1
  echo "ATTO_NATS_LLVM_NEGATIVE_$kind=PASS"
done
runtime="$root/src/main/cpp/oreslang/runtime"
for opt in 0 2; do
  "$clang" "-O$opt" -x ir "$scratch/subjects.ll" -x c -std=c11 \
    -I "$root/src/main/cpp" \
    "$runtime/NativeStringRegion.c" "$runtime/NativeIntArrayRegion.c" \
    "$runtime/NativeStringSplit.c" "$runtime/NativeRecordRegion.c" \
    -pthread -o "$scratch/subjects-O$opt"
  # Pipe output even if future guest diagnostics enable nonblocking stdout.
  "$scratch/subjects-O$opt" 2>"$scratch/stderr-O$opt" |
      cat >"$scratch/stdout-O$opt"
  [[ ! -s "$scratch/stderr-O$opt" && ! -s "$scratch/stdout-O$opt" ]]
  echo "ATTO_NATS_CANONICAL_SUBJECTS_LLVM_O${opt}_NATIVE_GUEST_PASS"
done
echo "LLVM_FULL_NATS_BROKER_NOT_TESTED=true"
echo "NATS_SPEEDUP_NOT_MEASURED=true"
