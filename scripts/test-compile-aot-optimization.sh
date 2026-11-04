#!/usr/bin/env bash
# HOST-PIPELINE ONLY: validates the Clang O0/O2 adapter using synthetic LLVM IR.
# The stub is NOT an AttoLang compiler and this is NOT guest parity evidence.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
real_clang="$(command -v clang)" || { echo 'Clang required for host test' >&2; exit 69; }
tmp="$(mktemp -d)"
trap 'rm -rf -- "$tmp"' EXIT
mkdir -p "$tmp/program fixtures" "$tmp/bin"
source="$tmp/program fixtures/scalar source.atto"
printf '%s\n' 'pub fnc main(): int { return 42; }' > "$source"
cp "$source" "$tmp/pristine-source"
cat > "$tmp/stand-in-compiler" <<'COMPILER'
#!/usr/bin/env bash
set -euo pipefail
[[ "$#" == 2 && "$1" == "--target=host" && "$2" == "$ATTO_EXPECTED_SOURCE" ]] || exit 76
cat <<'IR'
define i32 @main() {
entry:
  ret i32 42
}
IR
COMPILER
cat > "$tmp/no-gc-auditor" <<'AUDITOR'
#!/usr/bin/env bash
# Synthetic host-only auditor. Production IR acceptance uses the Rust auditor.
set -euo pipefail
[[ "$#" == 1 && -s "$1" ]] || exit 72
printf '%s\n' "$(sha256sum "$1" | cut -d' ' -f1)" >> "$ATTO_AUDITOR_LOG"
if [[ "${ATTO_AUDITOR_REJECT:-0}" == 1 ]]; then
  echo 'synthetic no-GC auditor rejected IR' >&2
  exit 73
fi
AUDITOR
cat > "$tmp/clang-recorder" <<'CLANG'
#!/usr/bin/env bash
set -euo pipefail
printf '%s\n' "$@" > "$ATTO_CLANG_ARGV_LOG"
exec "$ATTO_REAL_CLANG" "$@"
CLANG
chmod 700 "$tmp/stand-in-compiler" "$tmp/clang-recorder" "$tmp/no-gc-auditor"
export ORESLANG_LLVMC="$tmp/stand-in-compiler"
export CLANG="$tmp/clang-recorder"
export ATTO_REAL_CLANG="$real_clang"
export ATTO_EXPECTED_SOURCE="$source"
export ATTO_CLANG_ARGV_LOG="$tmp/clang-argv"
export ORES_NO_GC_IR_AUDIT="$tmp/no-gc-auditor"
export ATTO_AUDITOR_LOG="$tmp/auditor-argv"
for level in 0 2; do
    export ORESLANG_LLVM_AOT_OPT_LEVEL="$level"
    output="$tmp/bin/native-$level"
    "$root/scripts/compile-aot.sh" "$source" "$output" > "$tmp/compiler-stdout"
    [[ -x "$output" ]] || { echo "missing O$level executable" >&2; exit 1; }
    grep -Fqx -- "-O$level" "$ATTO_CLANG_ARGV_LOG" ||
        { echo "Clang did not receive -O$level" >&2; exit 1; }
    set +e
    "$output"
    result=$?
    set -e
    [[ "$result" == 42 ]] || { echo "synthetic IR native O$level exited $result, not 42" >&2; exit 1; }
    cmp -s "$source" "$tmp/pristine-source" || { echo 'source rewritten' >&2; exit 1; }
done
unset ORESLANG_LLVM_AOT_OPT_LEVEL
"$root/scripts/compile-aot.sh" "$source" "$tmp/bin/default" > /dev/null
grep -Fqx -- '-O2' "$ATTO_CLANG_ARGV_LOG" ||
    { echo 'legacy default did not remain O2' >&2; exit 1; }
for invalid in 'O2' '-O2' '3' '2;touch'; do
    export ORESLANG_LLVM_AOT_OPT_LEVEL="$invalid"
    if "$root/scripts/compile-aot.sh" "$source" "$tmp/bin/invalid"         > "$tmp/invalid.stdout" 2> "$tmp/invalid.stderr"; then
        echo "unsafe optimization setting was accepted: $invalid" >&2
        exit 1
    fi
    grep -Fq 'must be 0 or 2' "$tmp/invalid.stderr" ||
        { echo 'invalid setting lacked diagnostic' >&2; exit 1; }
    [[ ! -e "$tmp/bin/invalid" ]] ||
        { echo 'invalid setting published an executable' >&2; exit 1; }
done
unset ORESLANG_LLVM_AOT_OPT_LEVEL
if "$root/scripts/compile-aot.sh" "$source" "$source" > /dev/null 2>&1; then
    echo 'source/output alias was accepted' >&2
    exit 1
fi
cmp -s "$source" "$tmp/pristine-source" || { echo 'source modified after alias attempt' >&2; exit 1; }
[[ "$(wc -l < "$ATTO_AUDITOR_LOG")" -ge 3 ]] || { echo 'no-GC auditor did not inspect each emitted IR' >&2; exit 1; }
# A rejected IR must never publish a replacement or damage a previous executable.
cp -- "$tmp/bin/default" "$tmp/bin/preserved"
preserved_sha="$(sha256sum "$tmp/bin/preserved" | cut -d' ' -f1)"
if ATTO_AUDITOR_REJECT=1 "$root/scripts/compile-aot.sh" "$source" "$tmp/bin/preserved" > "$tmp/audit-refused.out" 2> "$tmp/audit-refused.err"; then
    echo 'no-GC audit rejection was bypassed' >&2; exit 1
fi
grep -Fq 'synthetic no-GC auditor rejected IR' "$tmp/audit-refused.err" || { echo 'audit negative failed for another reason' >&2; exit 1; }
[[ "$(sha256sum "$tmp/bin/preserved" | cut -d' ' -f1)" == "$preserved_sha" ]] || { echo 'audit rejection clobbered previous executable' >&2; exit 1; }
if ORES_NO_GC_IR_AUDIT="$tmp/missing-auditor" "$root/scripts/compile-aot.sh" "$source" "$tmp/bin/missing-auditor-result" > /dev/null 2>&1; then
    echo 'missing native no-GC auditor was accepted' >&2; exit 1
fi
[[ ! -e "$tmp/bin/missing-auditor-result" ]] || { echo 'missing auditor published an artifact' >&2; exit 1; }
# Alias protection must cover the no-GC auditor as well as compiler/Clang.
# An older driver overwrote the auditing executable with an ELF here.
auditor_sha="$(sha256sum "$ORES_NO_GC_IR_AUDIT" | cut -d' ' -f1)"
for candidate in direct hardlink symlink; do
    output="$tmp/bin/auditor-$candidate"
    case "$candidate" in
        direct) output="$ORES_NO_GC_IR_AUDIT" ;;
        hardlink) ln -- "$ORES_NO_GC_IR_AUDIT" "$output" ;;
        symlink) ln -s -- "$ORES_NO_GC_IR_AUDIT" "$output" ;;
    esac
    if "$root/scripts/compile-aot.sh" "$source" "$output" > "$tmp/alias-$candidate.out" 2> "$tmp/alias-$candidate.err"; then
        echo "no-GC auditor $candidate alias was accepted" >&2; exit 1
    fi
    grep -Fq 'must not alias' "$tmp/alias-$candidate.err" || {
        echo "auditor $candidate alias rejected for wrong reason" >&2; exit 1;
    }
    [[ "$(sha256sum "$ORES_NO_GC_IR_AUDIT" | cut -d' ' -f1)" == "$auditor_sha" ]] || {
        echo "no-GC auditor changed via $candidate output alias" >&2; exit 1;
    }
done
printf '%s\n' 'HOST-PIPELINE-PASS|clang_o0=1|clang_o2=1|default_o2=1|invalid_settings=4|synthetic_auditor_checks=1|auditor_negative=1|auditor_alias_denials=3|guest_compiler=NOT_EXECUTED'
