#!/usr/bin/env bash
# Production .atto parser -> verified MIR -> LLVM IR -> no-GC screen
# -> real Clang executable. This script MUST run, not just be checked in.
set -euo pipefail
compiler="${1:?production compiler required}"
clang="${2:?Clang required}"
fixtures="${3:?source fixture directory required}"
auditor="${4:?source-built no-GC auditor required}"
[[ -f "$compiler" && -x "$compiler" && ! -L "$compiler" &&
   -f "$auditor" && -x "$auditor" && ! -L "$auditor" &&
   "$(basename "$auditor")" == oreslang-no-gc-ir-audit ]] || {
    echo 'FAIL: missing compiler or source-built no-GC auditor' >&2; exit 1
}
command -v "$clang" >/dev/null 2>&1 || {
    echo 'FAIL: missing Clang' >&2; exit 1
}
clang_executable="$(command -v "$clang")"
[[ -f "$clang_executable" && -x "$clang_executable" ]] || {
    echo 'FAIL: missing executable Clang binary' >&2; exit 1
}
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
if command -v sha256sum >/dev/null 2>&1; then
    digest_tool=(sha256sum)
elif command -v shasum >/dev/null 2>&1; then
    digest_tool=(shasum -a 256)
else
    echo 'FAIL: no SHA-256 artifact utility' >&2; exit 1
fi
hash_file() {
    local output token
    if ! output="$("${digest_tool[@]}" -- "$1")"; then
        echo 'FAIL: failed artifact SHA-256' >&2; return 1
    fi
    token="$(printf '%s\n' "$output" | cut -d ' ' -f 1)"
    if [[ ! "$token" =~ ^[0-9a-f]{64}$ ]]; then
        echo 'FAIL: malformed artifact SHA-256' >&2; return 1
    fi
    printf '%s\n' "$token"
}
compiler_sha="$(hash_file "$compiler")"
clang_sha="$(hash_file "$clang_executable")"
auditor_sha="$(hash_file "$auditor")"
require_tools_unchanged() {
    if [[ "$(hash_file "$compiler")" != "$compiler_sha" ||
          "$(hash_file "$clang_executable")" != "$clang_sha" ||
          "$(hash_file "$auditor")" != "$auditor_sha" ]]; then
        echo 'FAIL: compiler, Clang or source-built auditor changed during native guest proof' >&2
        exit 1
    fi
}
printf '%s\n' 'STRICT_NO_GC_IR_SCREENED_ONLY (linked binary and FFI audit still required)'     > "$tmp/expected-audit.txt"
printf 'declare ptr @GC_malloc(i64)\n' > "$tmp/known-gc.ll"
if "$auditor" "$tmp/known-gc.ll" > "$tmp/known-gc.out" 2> "$tmp/known-gc.err"; then
    echo 'FAIL: no-GC auditor admitted collector' >&2; exit 1
fi
grep -Fq STRICT_NO_GC_IR_REJECTED "$tmp/known-gc.err" || {
    echo 'FAIL: auditor negative not recognized' >&2; exit 1
}
[[ ! -s "$tmp/known-gc.out" ]] || exit 1
require_tools_unchanged
audit() {
    local file="$1"
    [[ -s "$file" && -f "$file" && ! -L "$file" && ! "$file" -ef "$auditor" ]] || {
        echo 'FAIL: missing, aliased or unsafe guest LLVM IR' >&2; exit 1
    }
    local source_sha
    source_sha="$(hash_file "$file")"
    "$auditor" "$file" > "$tmp/audit.out" 2> "$tmp/audit.err" || {
        echo 'FAIL: IR rejected by source-built no-GC auditor' >&2; exit 1
    }
    [[ ! -s "$tmp/audit.err" ]] || exit 1
    cmp -s "$tmp/expected-audit.txt" "$tmp/audit.out" || {
        echo 'FAIL: contaminated auditor success output' >&2; exit 1
    }
    if [[ "$(hash_file "$file")" != "$source_sha" ]]; then
        echo 'FAIL: emitted LLVM IR mutated during audit' >&2
        exit 1
    fi
    require_tools_unchanged
}
run_process() {
    python3 - "$1" "$2" "$3" "$4" <<'PY'
import hashlib
import os
import pathlib
import resource
import signal
import subprocess
import sys
import tempfile
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
guest = pathlib.Path(sys.argv[1])
if guest.is_symlink() or not guest.is_file() or not os.access(guest, os.X_OK):
    raise SystemExit("FAIL: guest output is not a real executable regular file")
with guest.open("rb") as file:
    magic = file.read(4)
if sys.platform.startswith("linux"):
    allowed = (bytes.fromhex("7f454c46"),)
elif sys.platform == "darwin":
    allowed = tuple(bytes.fromhex(x) for x in
                    ("feedface","feedfacf","cefaedfe","cffaedfe"))
else:
    raise SystemExit("FAIL: unsupported host executable format for native proof")
if magic not in allowed:
    raise SystemExit("FAIL: guest is not a native ELF/Mach-O executable")
before = hashlib.sha256(guest.read_bytes()).hexdigest()
def limits():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    resource.setrlimit(resource.RLIMIT_FSIZE, (4096, 4096))
    resource.setrlimit(resource.RLIMIT_CPU, (10, 10))
try:
    # Never capture unbounded guest output in Python process memory.
    with tempfile.TemporaryFile() as out, tempfile.TemporaryFile() as err:
        result = subprocess.run([str(guest)], stdout=out, stderr=err,
                                timeout=15, check=False, preexec_fn=limits)
        if out.tell() or err.tell():
            raise SystemExit("FAIL: native guest unexpectedly wrote stdout/stderr")
except (OSError, subprocess.TimeoutExpired) as exc:
    raise SystemExit(f"FAIL: guest not executed {sys.argv[3]} {sys.argv[4]}: {exc}")
if hashlib.sha256(guest.read_bytes()).hexdigest() != before:
    raise SystemExit("FAIL: native guest executable changed while running")
if sys.argv[2] == "exit42":
    if result.returncode != 42:
        raise SystemExit(f"FAIL: {sys.argv[3]} {sys.argv[4]} exited {result.returncode}, expected 42")
else:
    if result.returncode not in (-signal.SIGILL, -signal.SIGTRAP):
        raise SystemExit(f"FAIL: bounds guest had no real trap signal: {result.returncode}")
print(f"ATTO_MIXED_GUEST_EXECUTABLE_SHA256={before} fixture={sys.argv[3]} opt={sys.argv[4]}")
PY
}
for fixture in positive-bool-write.atto positive-swap-tail.atto; do
    fixture_path="$fixtures/$fixture"
    [[ -f "$fixture_path" && -s "$fixture_path" && ! -L "$fixture_path" ]] || {
        echo "FAIL: missing or unsafe .atto source $fixture" >&2; exit 1
    }
    fixture_sha="$(hash_file "$fixture_path")"
    "$compiler" "$fixture_path" > "$tmp/native.ll"
    [[ "$(hash_file "$fixture_path")" == "$fixture_sha" ]] || {
        echo "FAIL: guest source changed during compiler execution" >&2; exit 1
    }
    require_tools_unchanged
    echo "ATTO_MIXED_GUEST_SOURCE_SHA256=$fixture:$fixture_sha"
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
        require_tools_unchanged
        run_process "$tmp/native" exit42 "$fixture" "$level"
        require_tools_unchanged
    done
done
bounds_source="$fixtures/negative-bounds-trap.atto"
[[ -f "$bounds_source" && -s "$bounds_source" && ! -L "$bounds_source" ]] || {
    echo 'FAIL: missing/unsafe native bounds-trap source' >&2; exit 1
}
bounds_sha="$(hash_file "$bounds_source")"
"$compiler" "$bounds_source" > "$tmp/bounds.ll"
[[ "$(hash_file "$bounds_source")" == "$bounds_sha" ]] || {
    echo 'FAIL: bounds source changed during compiler execution' >&2; exit 1
}
require_tools_unchanged
echo "ATTO_MIXED_GUEST_SOURCE_SHA256=negative-bounds-trap.atto:$bounds_sha"
audit "$tmp/bounds.ll"
grep -Fq 'icmp ult i64 1, ' "$tmp/bounds.ll" || {
    echo 'FAIL: bounds guard missing from native LLVM IR' >&2; exit 1
}
for level in -O0 -O2; do
    "$clang" "$level" -x ir "$tmp/bounds.ll" -o "$tmp/bounds"
    require_tools_unchanged
    run_process "$tmp/bounds" signal negative-bounds-trap.atto "$level"
    require_tools_unchanged
done

while IFS='|' read -r fixture diagnostic; do
    fixture_path="$fixtures/$fixture"
    [[ -f "$fixture_path" && -s "$fixture_path" && ! -L "$fixture_path" ]] || {
        echo "FAIL: missing/unsafe negative .atto source $fixture" >&2; exit 1
    }
    fixture_sha="$(hash_file "$fixture_path")"
    if "$compiler" "$fixture_path" > "$tmp/rejected.ll" 2> "$tmp/error.txt"; then
        echo "FAIL: unsupported mixed guest compiled: $fixture" >&2; exit 1
    fi
    if [[ "$(hash_file "$fixture_path")" != "$fixture_sha" ]]; then
        echo "FAIL: compiler mutated negative source bytes" >&2; exit 1
    fi
    require_tools_unchanged
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
require_tools_unchanged
echo 'INVERTED_BOOL_I64_NATIVE_GUEST_PASS exit42=4 signals=2 source_rejects=5'
