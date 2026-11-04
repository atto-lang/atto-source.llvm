#!/usr/bin/env bash
# Source-free fault injection for the native AttoLang compiler/Clang gate.
# These are harness tests ONLY and must never count as guest execution evidence.
set -euo pipefail
if (( $# != 2 )); then
  echo "usage: $0 CLANG FIXTURE_DIR" >&2
  exit 64
fi
clang="$1"
fixtures="$2"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
gate="$root/atto_first_aot_import_parity.sh"
work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT
[[ -f "$gate" ]] || { echo "missing native parity gate" >&2; exit 1; }

cat > "$work/accept-all" <<'STUB'
#!/usr/bin/env bash
printf 'define i32 @main() {\nentry:\n  ret i32 42\n}\n'
STUB
cat > "$work/reject-all" <<'STUB'
#!/usr/bin/env bash
echo "synthetic rejection" >&2
exit 9
STUB
cat > "$work/emit-partial-on-failure" <<'STUB'
#!/usr/bin/env bash
source="${@: -1}"
if [[ ! -f "$(dirname "$source")/math.atto" && ! -f "$(dirname "$source")/math.ores" ]]; then
  printf 'define i32 @main() {\nentry:\n  ret i32 42\n}\n'
  echo "synthetic rejected import with invalid IR leakage" >&2
  exit 9
fi
printf 'define i32 @main() {\nentry:\n  ret i32 42\n}\n'
STUB
cat > "$work/empty-ir" <<'STUB'
#!/usr/bin/env bash
exit 0
STUB
cat > "$work/wrong-return" <<'STUB'
#!/usr/bin/env bash
printf 'define i32 @main() {\nentry:\n  ret i32 0\n}\n'
STUB
chmod +x "$work/accept-all" "$work/reject-all" "$work/emit-partial-on-failure" \
  "$work/empty-ir" "$work/wrong-return"

passed=0
must_reject() {
  local label="$1" expected="$2"
  shift 2
  if bash "$gate" "$@" > "$work/output" 2>&1; then
    echo "HARNESS_SELFTEST_FAIL=$label:unexpected-success" >&2
    exit 1
  fi
  if ! grep -Fq "$expected" "$work/output"; then
    echo "HARNESS_SELFTEST_FAIL=$label:unexpected-diagnostic" >&2
    cat "$work/output" >&2
    exit 1
  fi
  passed=$((passed + 1))
  echo "HARNESS_SELFTEST_REJECTED=$label"
}
must_reject accepted_invalid 'missing_dependency: invalid imported .atto source compiled successfully' \
  "$work/accept-all" "$clang" "$fixtures"
must_reject no_compiler_acceptance 'relative_main: compiler rejected canonical .atto source' \
  "$work/reject-all" "$clang" "$fixtures"
must_reject rejected_with_partial_ir 'missing_dependency: rejected .atto emitted partial/executable LLVM IR' \
  "$work/emit-partial-on-failure" "$clang" "$fixtures"
must_reject empty_ir_success 'relative_main: compiler returned success without LLVM IR' \
  "$work/empty-ir" "$clang" "$fixtures"
must_reject wrong_native_exit 'relative_main: native guest -O0 returned 0 instead of 42' \
  "$work/wrong-return" "$clang" "$fixtures"
must_reject missing_compiler 'real compiler unavailable' \
  "$work/missing" "$clang" "$fixtures"
must_reject missing_fixture 'missing checked-in .atto fixture' \
  "$work/accept-all" "$clang" "$work"
mkdir -p "$work/tampered-fixtures"
cp -- "$fixtures/main.atto" "$fixtures/math.atto" "$fixtures/explicit.atto" \
  "$work/tampered-fixtures/"
printf "# altered fixture bytes\n" >> "$work/tampered-fixtures/math.atto"
must_reject changed_import_fixture 'shared Java/LLVM fixture blob mismatch: math.atto' \
  "$work/accept-all" "$clang" "$work/tampered-fixtures"
[[ "$passed" -eq 8 ]] || exit 1
echo "ATTO_FIRST_AOT_GATE_SELFTEST=8/8_SOURCE_FREE"
