#!/usr/bin/env bash
# LLVM source-to-native admission tests for the canonical Oreslang stdout path.
# Requires the REAL oreslang-llvmc and the LLVM repository's compile-aot.sh.
# No mocks, host-language guest implementation, or parser-only PASS allowed.
set -euo pipefail
umask 077
export LC_ALL=C

[[ "$#" -eq 3 ]] || { echo "usage: native_orie_stdout_alias.sh <llvmc> <clang> <llvm-root>" >&2; exit 64; }
compiler="$1"
clang="$2"
root="$3"
[[ -x "$compiler" && -f "$root/scripts/compile-aot.sh" ]] ||
  { echo "Missing native compiler or pinned native AOT driver" >&2; exit 69; }
command -v "$clang" >/dev/null ||
  { echo "Missing native Clang linker" >&2; exit 69; }
scratch="$(mktemp -d)"
trap 'rm -rf -- "$scratch"' EXIT
checks=0

native_exact() {
  local name="$1" expected="$2" source="$3"
  local program="$scratch/$name.atto" executable="$scratch/$name.native"
  printf '%s\n' "$source" > "$program"
  ORESLANG_LLVMC="$compiler" CLANG="$clang" \
    bash "$root/scripts/compile-aot.sh" "$program" "$executable" \
    > "$scratch/$name.build.stdout" 2> "$scratch/$name.build.stderr" ||
      { echo "NATIVE_STDOUT_LINK_FAILED: $name" >&2
        cat "$scratch/$name.build.stderr" >&2; exit 1; }
  [[ -x "$executable" ]] || { echo "NATIVE_STDOUT_MISSING_BINARY: $name" >&2; exit 1; }
  local -a statuses=()
  # Guest stdout must be a pipe: the native nonblocking reactor rejects
  # a regular-file stdout descriptor. Capture through a separate process.
  set +e
  "$executable" 2> "$scratch/$name.guest.stderr" |
    cat > "$scratch/$name.guest.stdout"
  statuses=("${PIPESTATUS[@]}")
  set -e
  [[ "${statuses[0]}" -eq 0 && "${statuses[1]}" -eq 0 &&
     ! -s "$scratch/$name.guest.stderr" ]] ||
    { echo "NATIVE_STDOUT_GUEST_EXIT_FAILED: $name" >&2; exit 1; }
  printf '%s\n' "$expected" > "$scratch/$name.expected"
  cmp -s "$scratch/$name.expected" "$scratch/$name.guest.stdout" ||
    { echo "NATIVE_STDOUT_OUTPUT_MISMATCH: $name" >&2; exit 1; }
  checks=$((checks+1))
  echo "NATIVE_STDOUT_GUEST_EXACT: $name"
}

native_reject() {
  local name="$1" source="$2" diagnostic="$3"
  local program="$scratch/$name.atto" ir="$scratch/$name.ll"
  printf '%s\n' "$source" > "$program"
  local compile_rc=0
  "$compiler" --target=host "$program" \
       > "$ir" 2> "$scratch/$name.compiler.stderr" || compile_rc=$?
  [[ "$compile_rc" -eq 1 ]] ||
    { echo "NATIVE_STDOUT_NOT_A_SOURCE_REJECTION: $name rc=$compile_rc" >&2; exit 1; }
  [[ ! -s "$ir" ]] || { echo "NATIVE_STDOUT_PARTIAL_IR: $name" >&2; exit 1; }
  grep -q '^oreslang-llvmc: ' "$scratch/$name.compiler.stderr" &&
    grep -Fq -- "$diagnostic" "$scratch/$name.compiler.stderr" ||
    { echo "NATIVE_STDOUT_WRONG_DIAGNOSTIC: $name" >&2
      cat "$scratch/$name.compiler.stderr" >&2; exit 1; }
  checks=$((checks+1))
  echo "NATIVE_STDOUT_REJECTED: $name"
}

native_exact canonical "PASS:orie-stdio" \
  'pub routine main(): int { stdio.stdout.println("PASS:orie-stdio"); return 0; }'
native_exact canonical_multi "PASS:orie-stdio" \
  'pub routine main(): int { stdio.stdout.println("PASS:", "orie-stdio"); return 0; }'
native_exact legacy "PASS:orie-stdio" \
  'pub routine main(): int { stdio.println("PASS:orie-stdio"); return 0; }'

native_reject unknown_stdout_op \
  'pub routine main(): int { stdio.stdout.write("bad"); return 0; }' \
  'unsupported native stdio operation'
native_reject stderr_not_stdout \
  'pub routine main(): int { stdio.stderr.println("bad"); return 0; }' \
  'unsupported native stdio operation'
native_reject unknown_stdout_field \
  'pub routine main(): int { stdio.stdout.stderr.println("bad"); return 0; }' \
  'unsupported native stdio operation'

[[ "$checks" -eq 6 ]] ||
  { echo "NATIVE_STDOUT_INCOMPLETE_TEST_COUNT=$checks" >&2; exit 1; }
echo "NATIVE_STDOUT_EXACT_GUEST_TESTS=3; REJECTED_WITH_NO_IR=3"
