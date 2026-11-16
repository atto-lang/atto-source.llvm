#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/bin"
cat > "$tmp/bin/clang" <<'CLANG'
#!/usr/bin/env bash
set -euo pipefail
out=
while [[ $# -gt 0 ]]; do
  if [[ "$1" == -o ]]; then shift; out="$1"; fi
  shift || :
done
test -n "$out"
printf '#!/bin/sh\nexit 42\n' > "$out"
chmod +x "$out"
CLANG
cat > "$tmp/mock-compiler" <<'COMPILER'
#!/usr/bin/env bash
set -euo pipefail
experiment=0 target=host source=
for arg in "$@"; do
  case "$arg" in
    --experimental-owned-int) experiment=1 ;;
    --target=*) target="${arg#--target=}" ;;
    *.atto) source="$arg" ;;
  esac
done
if [[ "$source" == *reject-*.atto ]]; then
  case "${MOCK_CASE:-valid}" in
    crash) echo 'simulated signal' >&2; exit 139 ;;
    generic) echo 'unrelated syntax error' >&2; exit 1 ;;
    partial_ir) echo 'define i64 @invalid() { ret i64 1 }'; echo 'ATTO_BOX_MAIN_V1_REJECT' >&2; exit 1 ;;
    successful_negative) echo 'ATTO_BOX_MAIN_V1_REJECT' >&2; exit 0 ;;
  esac
  case "$source" in
    *reject-actor-name.atto|*reject-alias-name.atto) echo NATIVE_OWNERSHIP_INTRINSIC_RESERVED >&2 ;;
    *) echo 'ATTO_BOX_MAIN_V1_REJECT: mock rejection' >&2 ;;
  esac
  exit 1
fi
if [[ "$experiment" != 1 || "$target" != host ]]; then
  echo 'ATTO_BOX_MAIN_V1_REJECT: gate' >&2; exit 1
fi
printf '%s\n' 'declare i64 @oreslang_owned_i64_region_new()' 'declare i64 @oreslang_owned_i64_region_read()' 'declare void @oreslang_owned_i64_region_drop()' 'declare void @oreslang_owned_i64_region_close()'
if [[ "${MOCK_IR_CASE:-calls}" == declarations ]]; then exit 0; fi
printf 'define i64 @main() {\nentry:\n'
for operation in new read drop close; do
  if [[ "${MOCK_IR_CASE:-calls}" == no_drop && "$operation" == drop ]]; then continue; fi
  if [[ "$operation" == new || "$operation" == read ]]; then
    printf '  %%v_%s = call i64 @oreslang_owned_i64_region_%s()\n' "$operation" "$operation"
  else
    printf '  call void @oreslang_owned_i64_region_%s()\n' "$operation"
  fi
done
printf '  ret i64 42\n}\n'
COMPILER
chmod +x "$tmp/bin/clang" "$tmp/mock-compiler"
run_case() {
  local name="$1" ir_case="$2" mock_case="$3" expected_status="$4" expected_line="$5" status=0
  MOCK_IR_CASE="$ir_case" MOCK_CASE="$mock_case" PATH="$tmp/bin:$PATH" \
    bash tests/atto_box_main_v1.sh "$tmp/mock-compiler" > "$tmp/$name.log" 2>&1 || status=$?
  if [[ "$status" != "$expected_status" ]]; then
    echo "ORACLE_TEST_FAIL $name expected status=$expected_status got=$status" >&2
    cat "$tmp/$name.log" >&2; exit 1
  fi
  grep -Fq "$expected_line" "$tmp/$name.log" || {
    echo "ORACLE_TEST_FAIL $name missing expected diagnostic: $expected_line" >&2
    cat "$tmp/$name.log" >&2; exit 1;
  }
  echo "ORACLE_TEST_PASS $name"
}
run_case valid_calls calls valid 0 ATTO_MAIN_BOX_I64_AOT_PASS
run_case declarations_only declarations valid 1 'missing executed-region call: new'
run_case missing_drop no_drop valid 1 'missing executed-region call: drop'
run_case compiler_crash calls crash 1 'abnormal compiler exit 139'
run_case generic_error calls generic 1 'lacked expected semantic diagnostic'
run_case partial_ir calls partial_ir 1 'negative fixture emitted LLVM IR'
run_case false_success calls successful_negative 1 'unsafe Box source compiled'
echo ATTO_BOX_MAIN_V1_ORACLE_SELF_TEST_7_PASS
