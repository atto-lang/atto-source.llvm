#!/usr/bin/env bash
# Production .atto parser -> verified MIR -> LLVM IR -> no-GC screen
# -> real Clang executable. This script MUST run, not just be checked in.
set -euo pipefail
compiler="$1"
clang="$2"
fixtures="$3"
auditor="$4"
[[ -x "$compiler" && -x "$auditor" && ! -L "$auditor" &&
   "$(basename "$auditor")" == oreslang-no-gc-ir-audit ]] || {
    echo 'FAIL: missing compiler or source-built no-GC auditor' >&2; exit 1
}
command -v "$clang" >/dev/null 2>&1 || {
    echo 'FAIL: missing Clang' >&2; exit 1
}
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
printf 'declare ptr @GC_malloc(i64)\n' > "$tmp/known-gc.ll"
if "$auditor" "$tmp/known-gc.ll" > "$tmp/known-gc.out" 2> "$tmp/known-gc.err"; then
    echo 'FAIL: no-GC auditor admitted collector' >&2; exit 1
fi
grep -Fq STRICT_NO_GC_IR_REJECTED "$tmp/known-gc.err" || {
    echo 'FAIL: auditor negative not recognized' >&2; exit 1
}
[[ ! -s "$tmp/known-gc.out" ]] || exit 1
audit() {
    local file="$1"
    [[ -s "$file" ]] || { echo 'FAIL: missing guest LLVM IR' >&2; exit 1; }
    "$auditor" "$file" > "$tmp/audit.out" 2> "$tmp/audit.err" || {
        echo 'FAIL: IR rejected by source-built no-GC auditor' >&2; exit 1
    }
    [[ ! -s "$tmp/audit.err" ]] || exit 1
    grep -Fxq 'STRICT_NO_GC_IR_SCREENED_ONLY (linked binary and FFI audit still required)' \
        "$tmp/audit.out" || {
        echo 'FAIL: invalid auditor success marker' >&2; exit 1
    }
}
run_process() {
    python3 - "$1" "$2" "$3" "$4" <<'PY'
import resource
import signal
import subprocess
import sys
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
try:
    result = subprocess.run([sys.argv[1]], stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL, timeout=15, check=False)
except (OSError, subprocess.TimeoutExpired) as exc:
    raise SystemExit(f"FAIL: guest not executed {sys.argv[3]} {sys.argv[4]}: {exc}")
if sys.argv[2] == "exit42":
    if result.returncode != 42:
        raise SystemExit(f"FAIL: {sys.argv[3]} {sys.argv[4]} exited {result.returncode}, expected 42")
else:
    if result.returncode not in (-signal.SIGILL, -signal.SIGTRAP):
        raise SystemExit(f"FAIL: bounds guest had no real trap signal: {result.returncode}")
PY
}
for fixture in positive-bool-write.atto positive-swap-tail.atto; do
    "$compiler" "$fixtures/$fixture" > "$tmp/native.ll"
    audit "$tmp/native.ll"
    for marker in '%col0 = alloca [' ' x i1]' '%col1 = alloca [' \
        ' x i64]' 'load i1, ptr ' 'icmp ult i64 ' \
        'store volatile i1 0, ptr ' 'store volatile i64 0, ptr ' \
        'call void @llvm.trap()'; do
        grep -Fq "$marker" "$tmp/native.ll" || {
            echo "FAIL: missing typed memory or guard: $marker" >&2; exit 1
        }
    done
    for level in -O0 -O2; do
        "$clang" "$level" -x ir "$tmp/native.ll" -o "$tmp/native"
        run_process "$tmp/native" exit42 "$fixture" "$level"
    done
done
"$compiler" "$fixtures/negative-bounds-trap.atto" > "$tmp/bounds.ll"
audit "$tmp/bounds.ll"
grep -Fq 'icmp ult i64 1, ' "$tmp/bounds.ll" || {
    echo 'FAIL: bounds guard missing from native LLVM IR' >&2; exit 1
}
for level in -O0 -O2; do
    "$clang" "$level" -x ir "$tmp/bounds.ll" -o "$tmp/bounds"
    run_process "$tmp/bounds" signal negative-bounds-trap.atto "$level"
done

while IFS='|' read -r fixture diagnostic; do
    if "$compiler" "$fixtures/$fixture" > "$tmp/rejected.ll" 2> "$tmp/error.txt"; then
        echo "FAIL: unsupported mixed guest compiled: $fixture" >&2; exit 1
    fi
    if [[ -s "$tmp/rejected.ll" ]] || ! grep -Fq "$diagnostic" "$tmp/error.txt"; then
        echo "FAIL: wrong rejection or leaked IR for $fixture" >&2
        cat "$tmp/error.txt" >&2
        exit 1
    fi
done <<'NEGATIVES'
negative-bool-as-int.atto|requires true or false literal
negative-bool-to-int-return.atto|cannot read bool field
negative-immutable-bool-set.atto|immutable inverted field
negative-bool-write-int.atto|requires true or false literal
negative-f64-unimplemented.atto|precisely two i64 fields or bool/i64 fields
NEGATIVES
echo 'INVERTED_BOOL_I64_NATIVE_GUEST_PASS exit42=4 signals=2 source_rejects=5'
