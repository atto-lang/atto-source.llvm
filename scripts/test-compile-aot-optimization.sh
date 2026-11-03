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
cat > "$tmp/clang-recorder" <<'CLANG'
#!/usr/bin/env bash
set -euo pipefail
printf '%s\n' "$@" > "$ATTO_CLANG_ARGV_LOG"
exec "$ATTO_REAL_CLANG" "$@"
CLANG
chmod 700 "$tmp/stand-in-compiler" "$tmp/clang-recorder"
export ORESLANG_LLVMC="$tmp/stand-in-compiler"
export CLANG="$tmp/clang-recorder"
export ATTO_REAL_CLANG="$real_clang"
export ATTO_EXPECTED_SOURCE="$source"
export ATTO_CLANG_ARGV_LOG="$tmp/clang-argv"
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
printf '%s\n' 'HOST-PIPELINE-PASS|clang_o0=1|clang_o2=1|default_o2=1|invalid_settings=4|guest_compiler=NOT_EXECUTED'
