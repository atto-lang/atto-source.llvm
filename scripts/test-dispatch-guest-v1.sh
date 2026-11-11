#!/usr/bin/env bash
# Compiler-backed AttoLang dispatch v1: O0/O2 guests, no synthetic IR.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
cd "$root"
: "${EXPECTED_HEAD:?exact source head required}" "${ORESLANG_LLVMC:?real compiler required}" "${ORES_NO_GC_IR_AUDIT:?no-GC auditor required}" "${CLANG:?Clang required}"
test "$(git rev-parse HEAD)" = "$EXPECTED_HEAD"
test -z "$(git status --porcelain)"
compiler="$(realpath -e -- "$ORESLANG_LLVMC")"
auditor="$(realpath -e -- "$ORES_NO_GC_IR_AUDIT")"
test -x "$compiler" && test -x "$auditor"
test "$(basename "$compiler")" = "oreslang-llvmc"
command -v "$CLANG" >/dev/null
tmp="$(mktemp -d)"
trap 'rm -rf -- "$tmp"' EXIT
printf 'DISPATCH_LLVM_SOURCE_HEAD=%s|tree=%s|compiler_sha256=%s\n' "$EXPECTED_HEAD" "$(git rev-parse 'HEAD^{tree}')" "$(sha256sum "$compiler" | cut -d' ' -f1)"
base="$root/contracts/dispatch-v1"
test -f "$base/cases.tsv"
test -f "$base/README.md"
for case_name in direct_call_42 instance_call_42; do
  source="$base/$case_name.atto"
  test -f "$source" && test ! -L "$source"
  original="$(sha256sum "$source" | cut -d' ' -f1)"
  for level in 0 2; do
    export ORESLANG_LLVM_AOT_OPT_LEVEL="$level"
    ir="$tmp/$case_name-$level.ll"
    "$compiler" --target=host "$source" > "$ir"
    test -s "$ir" && grep -Eq '^define .*@' "$ir"
    "$auditor" "$ir" >/dev/null
    executable="$tmp/$case_name-$level"
    "$root/scripts/compile-aot.sh" "$source" "$executable" >/dev/null
    test -x "$executable"
    code=0
    timeout --signal=TERM 15s "$executable" > "$tmp/$case_name-$level.stdout" 2>"$tmp/$case_name-$level.stderr" || code=$?
    test "$code" = 42 || { echo "unexpected native guest exit $case_name O$level: $code" >&2; exit 1; }
    test ! -s "$tmp/$case_name-$level.stderr"
    test ! -s "$tmp/$case_name-$level.stdout"
    test "$(sha256sum "$source" | cut -d' ' -f1)" = "$original"
    printf 'DISPATCH_LLVM_NATIVE_GUEST_PASS|case=%s|O%s|head=%s|source_blob=%s|exit=42|ir_sha256=%s|binary_sha256=%s\n' \
      "$case_name" "$level" "$EXPECTED_HEAD" "$(git hash-object "$source")" \
      "$(sha256sum "$ir" | cut -d' ' -f1)" "$(sha256sum "$executable" | cut -d' ' -f1)"
  done
done
for case_name in static_qualified_42 virtual_override_121020 wrong_arity_reject; do
  source="$base/$case_name.atto"
  for level in 0 2; do
    export ORESLANG_LLVM_AOT_OPT_LEVEL="$level"
    ir="$tmp/reject-$case_name-$level.ll"
    if "$compiler" --target=host "$source" >"$ir" 2>"$tmp/reject-$case_name-$level.stderr"; then
      echo "previously unsupported/invalid source unexpectedly accepted: $case_name" >&2
      exit 1
    fi
    test ! -s "$ir" || { echo "compiler leaked partial IR for $case_name" >&2; exit 1; }
    test -s "$tmp/reject-$case_name-$level.stderr"
    test ! -e "$tmp/reject-$case_name-$level"
    printf 'DISPATCH_LLVM_FAIL_CLOSED|case=%s|O%s|source_blob=%s|no_ir=TRUE|status=UNSUPPORTED_OR_INVALID_NOT_PARITY\n' \
      "$case_name" "$level" "$(git hash-object "$source")"
  done
done
unset ORESLANG_LLVM_AOT_OPT_LEVEL
echo 'DISPATCH_LLVM_V1_MATRIX=EXECUTED_DIRECT_AND_INSTANCE_ONLY;VIRTUAL_AND_STATIC=NOT_IMPLEMENTED'
