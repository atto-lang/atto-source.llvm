#!/usr/bin/env bash
# HOST-ONLY adversarial test of the real f64 Bash admission harness.
# No AttoLang compiler guest, linked LLVM guest, or backend parity is executed.
set -euo pipefail
umask 077
export LC_ALL=C
[[ "$#" -eq 1 && -f "$1" ]] ||
  { echo "usage: native_f64_gate_security_selftest.sh <real-harness-path>" >&2; exit 64; }
harness="$1"
scratch="$(mktemp -d)"
trap 'rm -rf -- "$scratch"' EXIT

cat > "$scratch/mock-compiler" <<'MOCK'
#!/usr/bin/env bash
set -euo pipefail
[[ "$#" -eq 2 && "$1" == "--target=host" && -s "$2" ]] || exit 70
case "$MOCK_MODE" in
  comment_only)
    cat <<'IR'
; This comment is deliberately forged and MUST NOT count as typed LLVM IR.
; %fake = alloca double, align 8
define i32 @main() {
  ret i32 42
}
IR
    ;;
  wrong_entry)
    cat <<'IR'
define i32 @wrong_main() {
  %real = alloca double, align 8
  ret i32 42
}
IR
    ;;
  typed_link_fail|constant_42|script_shim)
    cat <<'IR'
define i32 @main() {
  %real = alloca double, align 8
  store double 1.000000e+00, ptr %real
  ret i32 42
}
IR
    ;;
  *) exit 71 ;;
esac
MOCK
cat > "$scratch/mock-clang" <<'MOCK'
#!/usr/bin/env bash
set -euo pipefail
[[ "$#" -ge 5 ]] || exit 70
if [[ "$MOCK_MODE" == typed_link_fail ]]; then exit 88; fi
[[ "$MOCK_MODE" == constant_42 || "$MOCK_MODE" == script_shim ]] || exit 71
output=""
while (( $# > 0 )); do
  if [[ "$1" == -o && $# -ge 2 ]]; then output="$2"; shift 2; else shift; fi
done
[[ -n "$output" ]] || exit 72
if [[ "$MOCK_MODE" == script_shim ]]; then
  printf '#!/bin/sh\nexit 42\n' > "$output"
else
  [[ -f "$HOST_CONSTANT42_IMAGE" ]] || exit 73
  cp "$HOST_CONSTANT42_IMAGE" "$output"
fi
chmod 700 "$output"
MOCK
chmod 700 "$scratch/mock-compiler" "$scratch/mock-clang"
# A real host C compiler produces an executable machine-code constant-42
# fixture, deliberately NOT an AttoLang compiler or guest.
command -v cc >/dev/null 2>&1 ||
  { echo "HOST_F64_CC_MISSING" >&2; exit 69; }
cat > "$scratch/constant42.c" <<'C'
int main(void) { return 42; }
C
cc -O0 "$scratch/constant42.c" -o "$scratch/constant42"
[[ -x "$scratch/constant42" ]] || exit 69
export HOST_CONSTANT42_IMAGE="$scratch/constant42"

reject() {
  local label="$1" mode="$2" diagnostic="$3"
  local status=0
  MOCK_MODE="$mode" bash "$harness" "$scratch/mock-compiler" "$scratch/mock-clang" \
    > "$scratch/$label.out" 2> "$scratch/$label.err" || status=$?
  [[ "$status" -eq 1 ]] ||
    { echo "HOST_F64_MOCK_NOT_REJECTED: $label rc=$status" >&2; exit 1; }
  grep -Fq -- "$diagnostic" "$scratch/$label.err" ||
    { echo "HOST_F64_MOCK_WRONG_ERROR: $label" >&2
      cat "$scratch/$label.err" >&2; exit 1; }
  ! grep -Fq 'NATIVE_F64_SCALAR_GUESTS_PASS' "$scratch/$label.out" ||
    { echo "HOST_F64_MOCK_PROMOTED: $label" >&2; exit 1; }
  echo "HOST_ONLY_F64_HARNESS_REJECTED: $label"
}

reject comment_only comment_only 'NATIVE_F64_NO_TYPED_DOUBLE_IR: scalar_local'
reject wrong_entry wrong_entry 'NATIVE_F64_MISSING_NATIVE_MAIN: scalar_local'
reject failed_link typed_link_fail 'NATIVE_F64_LINK_FAILED: scalar_local O0'
reject shell_native_shim script_shim 'NATIVE_F64_LINK_NOT_NATIVE_IMAGE: scalar_local O0'
reject fabricated_constant_42 constant_42 'NATIVE_F64_EXIT_MISMATCH: f64_nested_local_read_exit_37 O0 rc=42 expected=37'
echo "HOST_ONLY_F64_GATE_ADVERSARIAL_PASS=5; NO_ATTO_COMPILER_OR_GUEST_EXECUTED"
