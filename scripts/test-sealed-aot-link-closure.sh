#!/usr/bin/env bash
# Source-free mock compiler/IR auditor/Clang regression. This verifies
# *only* the wrapper's refusal to link unapproved native dependencies.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
tmp="$(mktemp -d)"
trap 'rm -rf -- "$tmp"' EXIT
printf 'fnc main(): int { return 42; }\n' > "$tmp/main.atto"
cat > "$tmp/mock-llvmc" <<'MOCK_COMPILER'
#!/usr/bin/env bash
set -euo pipefail
if [[ "${ATTO_TEST_PROFILE:-off}" != off ]]; then
  [[ "${1:-}" == "--aot-profile=${ATTO_TEST_PROFILE}" ]] || exit 42
fi
: > "$ATTO_TEST_COMPILER_MARKER"
printf 'declare void @%s()\n' "${ATTO_TEST_SYMBOL:-none}"
printf 'define i64 @main() { ret i64 42 }\n'
MOCK_COMPILER
cat > "$tmp/mock-auditor" <<'MOCK_AUDITOR'
#!/usr/bin/env bash
set -euo pipefail
[[ -s "${1:-}" ]] || exit 43
: > "$ATTO_TEST_AUDITOR_MARKER"
MOCK_AUDITOR
cat > "$tmp/mock-clang" <<'MOCK_CLANG'
#!/usr/bin/env bash
set -euo pipefail
: > "$ATTO_TEST_CLANG_MARKER"
exit 89
MOCK_CLANG
chmod +x "$tmp/mock-llvmc" "$tmp/mock-auditor" "$tmp/mock-clang"
cases=0
for profile in sealed-scalar-v1 sealed-class-fields-v1; do
  for symbol in ores_string_new ores_record_new oreslang_int_array_new \
      oreslang_source_actor_spawn oreslang_source_task_start_i64; do
    rm -f "$tmp/out" "$tmp/compiler-called" "$tmp/auditor-called" "$tmp/clang-called"
    if ATTO_TEST_PROFILE="$profile" ATTO_TEST_SYMBOL="$symbol" \
       ATTO_TEST_COMPILER_MARKER="$tmp/compiler-called" \
       ATTO_TEST_AUDITOR_MARKER="$tmp/auditor-called" \
       ATTO_TEST_CLANG_MARKER="$tmp/clang-called" \
       ORESLANG_LLVM_AOT_PROFILE="$profile" \
       ORESLANG_LLVMC="$tmp/mock-llvmc" \
       ORES_NO_GC_IR_AUDIT="$tmp/mock-auditor" \
       CLANG="$tmp/mock-clang" \
       bash "$root/scripts/compile-aot.sh" "$tmp/main.atto" "$tmp/out" \
           > "$tmp/stdout" 2> "$tmp/stderr"; then
      echo "FAIL: $profile admitted unverified runtime dependency $symbol" >&2
      exit 1
    else
      rc="$?"
    fi
    [[ "$rc" == 65 ]] || { echo "FAIL: expected code 65, got $rc ($symbol)" >&2; exit 1; }
    grep -Fq 'sealed AOT refuses unchecked native runtime dependency' "$tmp/stderr" || {
      echo "FAIL: missing or incorrect refusal diagnostic ($symbol)" >&2; exit 1;
    }
    [[ -f "$tmp/compiler-called" && -f "$tmp/auditor-called" ]] || {
      echo "FAIL: dependency guard bypassed compiler or IR auditor ($symbol)" >&2; exit 1;
    }
    [[ ! -e "$tmp/clang-called" && ! -e "$tmp/out" && ! -s "$tmp/stdout" ]] || {
      echo "FAIL: rejected dependency reached Clang or output ($symbol)" >&2; exit 1;
    }
    cases=$((cases + 1))
  done
done
# Non-sealed mode preserves its old linker path. The fake Clang exits 89:
# this is a deliberate host-only negative control, not a compiled guest.
rm -f "$tmp/out" "$tmp/compiler-called" "$tmp/auditor-called" "$tmp/clang-called"
if ATTO_TEST_PROFILE=off ATTO_TEST_SYMBOL=ores_string_new \
   ATTO_TEST_COMPILER_MARKER="$tmp/compiler-called" \
   ATTO_TEST_AUDITOR_MARKER="$tmp/auditor-called" \
   ATTO_TEST_CLANG_MARKER="$tmp/clang-called" \
   ORESLANG_LLVM_AOT_PROFILE=off \
   ORESLANG_LLVMC="$tmp/mock-llvmc" \
   ORES_NO_GC_IR_AUDIT="$tmp/mock-auditor" \
   CLANG="$tmp/mock-clang" \
   bash "$root/scripts/compile-aot.sh" "$tmp/main.atto" "$tmp/out" \
       > "$tmp/stdout" 2> "$tmp/stderr"; then
  echo 'FAIL: fake ordinary linker unexpectedly succeeded' >&2
  exit 1
else
  rc="$?"
fi
[[ "$rc" == 89 && -f "$tmp/clang-called" ]] || {
  echo "FAIL: ordinary AOT path no longer reaches native linker ($rc)" >&2
  exit 1
}
[[ "$cases" == 10 ]] || { echo "FAIL: expected ten negatives; found $cases" >&2; exit 1; }
echo "PASS: $cases source-free sealed link-closure negatives; ordinary linker preserved"
