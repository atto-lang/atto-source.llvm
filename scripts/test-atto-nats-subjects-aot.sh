#!/usr/bin/env bash
# Exact application-owned AttoNATS subject validation source -> actual native
# oreslang-llvmc -> audited LLVM IR -> independently linked Clang O0/O2 guests.
# No compiler export or private destination checkout. No Java/host broker.
set -euo pipefail
[[ $# == 3 ]] || { echo 'usage: test-atto-nats-subjects-aot.sh llvmc clang auditor' >&2; exit 64; }
compiler="$1"; clang="$2"; auditor="$3"
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
[[ -x "$compiler" && -x "$auditor" ]] || { echo 'NATIVE_LLVM_COMPILER_OR_AUDITOR_MISSING' >&2; exit 69; }
command -v "$clang" >/dev/null || { echo 'CLANG_MISSING' >&2; exit 69; }
orig="$root/tests/fixtures/atto-nats/subjects.atto"
guest="$root/tests/fixtures/atto-nats/native_subjects_guest.atto"
verify_blob() {
    local path="$1" expected="$2"
    [[ -f "$path" ]] || { echo 'MISSING_CANONICAL_APP_SOURCE' >&2; exit 65; }
    [[ "$(git -C "$root" hash-object "$path")" == "$expected" ]] || {
        echo 'APP_SOURCE_CONTENT_DIVERGED' >&2; exit 65;
    }
    [[ "$(git -C "$root" rev-parse "HEAD:${path#"$root/"}")" == "$expected" ]] || {
        echo 'APP_SOURCE_COMMIT_BLOB_DIVERGED' >&2; exit 65;
    }
}
verify_blob "$orig" 538a98c2ec79a4d859cd41ac6b85fde9f62e6869
verify_blob "$guest" 36a1945e5d100225f36d46728157414e55eea0b3
[[ "$(head -n 1 "$orig")" == 'use namespace core;' ]] || exit 65
[[ "$(head -n 1 "$guest")" == 'use namespace guest;' ]] || exit 65
scratch="$(mktemp -d)"
trap 'rm -rf -- "$scratch"' EXIT
# The existing native LLVM front end has no executable namespace header.
# Only the exactly checked first line of each file is removed. All functional
# subject validation methods remain byte-identical Atto source.
{
    tail -n +2 "$orig"
    printf '\n'
    tail -n +2 "$guest"
} >"$scratch/subjects.atto"
echo 'ATTO_NATS_SUBJECTS_APP_BLOB=538a98c2ec79a4d859cd41ac6b85fde9f62e6869'
echo 'NATIVE_NAMESPACE_NORMALIZATION=FIRST_LINE_ONLY'
echo 'VERBATIM_MULTI_FILE_NAMESPACE_EXECUTED=false'
"$compiler" --target=host "$scratch/subjects.atto" \
    >"$scratch/subjects.ll" 2>"$scratch/compiler.stderr" || {
    echo 'CANONICAL_ATTO_NATS_SUBJECT_VALIDATION_NATIVE_COMPILE=FAIL' >&2
    head -n 20 "$scratch/compiler.stderr" >&2
    exit 1
}
[[ -s "$scratch/subjects.ll" && ! -s "$scratch/compiler.stderr" ]]
grep -Eq '^define i32 @main\(' "$scratch/subjects.ll"
grep -Fq '@ores_string_split_literal_fill' "$scratch/subjects.ll"
grep -Fq '@ores_string_contains_literal' "$scratch/subjects.ll"
grep -Fq '@oreslang_int_array_get' "$scratch/subjects.ll"
"$auditor" "$scratch/subjects.ll" >/dev/null
echo 'CANONICAL_ATTO_NATS_SUBJECT_VALIDATION_NATIVE_IR_AND_AUDIT=PASS'
# Compile-negative: any altered subject delimiter type must produce no IR.
sed 's/split_literal("\.")/split_literal(42)/g' "$scratch/subjects.atto" >"$scratch/bad.atto"
grep -Fq 'split_literal(42)' "$scratch/bad.atto"
if "$compiler" --target=host "$scratch/bad.atto" \
        >"$scratch/bad.ll" 2>"$scratch/bad.stderr"; then
    echo 'UNSOUND_SUBJECT_DELIMITER_TYPE_ADMITTED' >&2; exit 1
fi
[[ ! -s "$scratch/bad.ll" && -s "$scratch/bad.stderr" ]] || {
    echo 'BAD_GUEST_GENERATED_PARTIAL_IR' >&2; exit 1;
}
printf 'ATTO_NATS_CANONICAL_SUBJECTS_LLVM_NATIVE_PASS\n' >"$scratch/expected.out"
for level in 0 2; do
    ORESLANG_LLVMC="$compiler" \
    ORES_NO_GC_IR_AUDIT="$auditor" \
    ORESLANG_LLVM_AOT_OPT_LEVEL="$level" \
    CLANG="$clang" \
       bash "$root/scripts/compile-aot.sh" "$scratch/subjects.atto" "$scratch/subjects-O$level" \
       >"$scratch/build-O$level.log"
    [[ -s "$scratch/subjects-O$level" ]]
    # NativeStringRegion's nonblocking writer requires a pipe, not a file.
    "$scratch/subjects-O$level" 2>"$scratch/err-O$level" | cat >"$scratch/out-O$level"
    [[ ! -s "$scratch/err-O$level" ]]
    cmp "$scratch/expected.out" "$scratch/out-O$level"
    echo "NATIVE_ATTO_NATS_SUBJECTS_LLVM_O${level}_EXECUTED=PASS"
done
echo 'CANONICAL_ATTO_NATS_BROKER_EXECUTED=false'
echo 'HARD_REALTIME_AND_PERFORMANCE_PARITY_NOT_CERTIFIED=true'
