#!/usr/bin/env bash
# Restricted real .atto -> production LLVM compiler -> LLVM IR -> Clang native.
# This is NOT a host reference, fake compiler, JIT, actor or SIMD qualification.
set -euo pipefail
compiler="${1:?compiler}"
clang="${2:?clang}"
fixtures="${3:?mixed source fixture directory}"
auditor="${4:?source-built IR audit executable}"
if [[ ! -f "$compiler" || ! -x "$compiler" || -L "$compiler" ||
      ! -f "$auditor" || ! -x "$auditor" || -L "$auditor" ||
      "${auditor##*/}" != "oreslang-no-gc-ir-audit" ]]; then
    echo 'FAIL: missing/nonexecutable or symlinked source-built compiler/auditor' >&2
    exit 1
fi
command -v "$clang" >/dev/null 2>&1 || { echo 'FAIL: Clang not found' >&2; exit 1; }
command -v python3 >/dev/null 2>&1 || { echo 'FAIL: Python3 required for signal checks' >&2; exit 1; }
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

if command -v sha256sum >/dev/null 2>&1; then
    hash_tool=(sha256sum)
elif command -v shasum >/dev/null 2>&1; then
    hash_tool=(shasum -a 256)
else
    echo 'FAIL: SHA-256 utility unavailable' >&2
    exit 1
fi
hash_file() {
    local data digest
    data="$("${hash_tool[@]}" -- "$1")" || return 1
    digest="${data%% *}"
    [[ "$digest" =~ ^[0-9a-f]{64}$ ]] || return 1
    printf '%s\n' "$digest"
}
# Distribution Clang wrappers are often symlinks. Resolve once and
# execute the verified target, not a subsequently retargeted command alias.
clang_path="$(command -v "$clang")"
clang_binary="$(readlink -f "$clang_path")"
[[ -n "$clang_binary" && -f "$clang_binary" && -x "$clang_binary" &&
   ! -L "$clang_binary" ]] || {
    echo 'FAIL: unable to pin concrete Clang executable' >&2; exit 1
}
compiler_sha="$(hash_file "$compiler")" || exit 1
clang_sha="$(hash_file "$clang_binary")" || exit 1
auditor_sha="$(hash_file "$auditor")" || exit 1
require_tools_unchanged() {
    [[ "$(hash_file "$compiler")" == "$compiler_sha" &&
       "$(hash_file "$clang_binary")" == "$clang_sha" &&
       "$(hash_file "$auditor")" == "$auditor_sha" ]] || {
        echo 'FAIL: source compiler, Clang or no-GC auditor mutated' >&2; exit 1
    }
}
printf '%s\n' 'STRICT_NO_GC_IR_SCREENED_ONLY (linked binary and FFI audit still required)' > "$tmp/expected-audit.txt"
printf '%s\n' 'declare ptr @GC_malloc(i64)' > "$tmp/known-gc.ll"
if "$auditor" "$tmp/known-gc.ll" >"$tmp/known-gc.out" 2>"$tmp/known-gc.err"; then
    echo 'FAIL: source-built auditor admitted collector import' >&2
    exit 1
fi
grep -Fq 'STRICT_NO_GC_IR_REJECTED' "$tmp/known-gc.err" ||
    { echo 'FAIL: missing known-GC diagnostic' >&2; exit 1; }
[[ ! -s "$tmp/known-gc.out" ]] || {
    echo 'FAIL: no-GC auditor emitted stdout on rejection' >&2; exit 1
}
require_tools_unchanged

audit_ir() {
    local file="$1" before after
    before="$(hash_file "$file")" || exit 1
    [[ -s "$file" ]] || { echo 'FAIL: empty guest IR' >&2; exit 1; }
    if ! "$auditor" "$file" >"$tmp/audit.out" 2>"$tmp/audit.err"; then
        echo 'FAIL: mixed source IR failed mandatory no-GC audit' >&2
        cat "$tmp/audit.err" >&2
        exit 1
    fi
    after="$(hash_file "$file")" || exit 1
    if ! cmp -s "$tmp/expected-audit.txt" "$tmp/audit.out" ||
       [[ -s "$tmp/audit.err" || "$before" != "$after" ]]; then
        echo 'FAIL: mixed source IR audit or provenance changed' >&2
        exit 1
    fi
    require_tools_unchanged
    audited_ir_sha="$before"
}
compile_guest() {
    local fixture="$1" ir="$2" fixture_sha
    [[ -f "$fixture" && -s "$fixture" && ! -L "$fixture" ]] || {
        echo "FAIL: missing or unsafe .atto guest fixture $fixture" >&2; exit 1
    }
    fixture_sha="$(hash_file "$fixture")" || exit 1
    "$compiler" "$fixture" > "$ir"
    [[ "$(hash_file "$fixture")" == "$fixture_sha" ]] || {
        echo 'FAIL: source .atto fixture mutated during compiler execution' >&2; exit 1
    }
    require_tools_unchanged
    audit_ir "$ir"
    echo "MIXED_NATIVE_SOURCE_SHA256=$fixture_sha file=${fixture##*/}"
}
verify_compilation_unchanged() {
    local ir="$1"
    [[ "$(hash_file "$ir")" == "$audited_ir_sha" ]] || {
        echo 'FAIL: LLVM IR mutated after no-GC audit' >&2; exit 1
    }
    require_tools_unchanged
}
# Verify a *real* native process and bound its execution independently of CTest.
native_status() {
    python3 - "$1" "$2" <<'PY'
import hashlib
import os
from pathlib import Path
import resource
import signal
import subprocess
import sys
import tempfile
guest = Path(sys.argv[1])
if guest.is_symlink() or not guest.is_file() or not os.access(guest, os.X_OK):
    raise SystemExit("FAIL: guest image not an executable regular file")
with guest.open("rb") as f:
    magic = f.read(4)
if sys.platform.startswith("linux"):
    expected_magic = {bytes.fromhex("7f454c46")}
elif sys.platform == "darwin":
    expected_magic = {bytes.fromhex(x) for x in
                      ("feedface", "feedfacf", "cefaedfe", "cffaedfe")}
else:
    raise SystemExit("FAIL: native process verification unsupported on this host")
if magic not in expected_magic:
    raise SystemExit("FAIL: executable-format proof rejected forged guest")
before = hashlib.sha256(guest.read_bytes()).hexdigest()
def limits():
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    resource.setrlimit(resource.RLIMIT_CPU, (10, 10))
    resource.setrlimit(resource.RLIMIT_FSIZE, (4096, 4096))
try:
    with tempfile.TemporaryFile() as out, tempfile.TemporaryFile() as err:
        run = subprocess.run([str(guest)], stdout=out, stderr=err,
                             timeout=15, check=False, preexec_fn=limits)
        if out.tell() or err.tell():
            raise SystemExit("FAIL: native guest emitted unexpected stdout/stderr")
except (OSError, subprocess.TimeoutExpired) as exc:
    raise SystemExit(f"FAIL: native mixed guest did not complete: {exc}")
if hashlib.sha256(guest.read_bytes()).hexdigest() != before:
    raise SystemExit("FAIL: native executable changed during real process")
expected = sys.argv[2]
if expected == "exit42":
    if run.returncode != 42:
        raise SystemExit(f"FAIL: native mixed guest expected exit 42 got {run.returncode}")
elif expected == "trap":
    if run.returncode not in (-signal.SIGILL, -signal.SIGTRAP):
        raise SystemExit(f"FAIL: native mixed guest expected genuine trap got {run.returncode}")
else:
    raise SystemExit("FAIL: unknown native outcome")
print("MIXED_NATIVE_IMAGE_SHA256="+before+" expected="+expected)
PY
}
for file in positive-mixed-swap.atto positive-permuted-columns.atto; do
    compile_guest "$fixtures/$file" "$tmp/guest.ll"
    # The optimizer must see genuine field-separated LLVM arrays, typed loads
    # and checked terminal owner revocation, not a host-only pretend backend.
    for required in ' x i1]' ' x double]' 'store volatile i1 false, ptr %alive'                     'icmp ult i64' 'call void @llvm.trap()'                     'store volatile double'; do
        grep -Fq "$required" "$tmp/guest.ll" ||
            { echo "FAIL: $file omits typed IR check: $required" >&2; exit 1; }
    done
    # Source-typed f64 payload must survive AST->MIR->LLVM IR as exact bits;
    # a correct i64 exit alone would not observe a silently dropped f64 store.
    if [[ "$file" == "positive-mixed-swap.atto" ]]; then
        required_f64='0x400E000000000000' # 3.75, mutable set
    else
        required_f64='0x4002000000000000' # 2.25, second row
    fi
    grep -Fq "$required_f64" "$tmp/guest.ll" ||
        { echo "FAIL: mixed f64 source payload lost in native IR: $file" >&2; exit 1; }
    if [[ "$file" == "positive-permuted-columns.atto" ]]; then
        # A real -0.0 source literal must retain the IEEE sign bit when
        # assigned to an active f64 column. Only *inactive* -0.0 is forbidden.
        grep -Fq '0x8000000000000000' "$tmp/guest.ll" ||
            { echo 'FAIL: active f64 negative-zero sign bit lost in LLVM IR' >&2; exit 1; }
    fi
    for opt in -O0 -O2; do
        "$clang_binary" "$opt" -x ir "$tmp/guest.ll" -o "$tmp/guest"
        verify_compilation_unchanged "$tmp/guest.ll"
        native_status "$tmp/guest" exit42
        verify_compilation_unchanged "$tmp/guest.ll"
    done
done
# The historical bool/i64 source profile uses this SAME mixed typed
# emitter and native test runner; it must never grow an independent ABI.
for file in pair-positive-bool-write.atto pair-positive-swap-tail.atto; do
    compile_guest "$fixtures/$file" "$tmp/pair.ll"
    for required in ' x i1]' ' x i64]' 'load i1, ptr ' \
                    'store volatile i1 false, ptr %alive' \
                    'store volatile i1 false, ptr ' \
                    'icmp ult i64' 'call void @llvm.trap()'; do
        grep -Fq "$required" "$tmp/pair.ll" || {
            echo "FAIL: pair guest omitted typed storage, owner guard or trap: $required" >&2
            exit 1
        }
    done
    if grep -Fq ' x double]' "$tmp/pair.ll"; then
        echo 'FAIL: bool/i64 guest emitted an invented f64 lane' >&2
        exit 1
    fi
    for opt in -O0 -O2; do
        "$clang_binary" "$opt" -x ir "$tmp/pair.ll" -o "$tmp/pair"
        verify_compilation_unchanged "$tmp/pair.ll"
        native_status "$tmp/pair" exit42
        verify_compilation_unchanged "$tmp/pair.ll"
    done
done
compile_guest "$fixtures/negative-stale-dense-index.atto" "$tmp/trap.ll"
grep -Fq 'call void @llvm.trap()' "$tmp/trap.ll" ||
    { echo 'FAIL: missing out-of-bounds trap in mixed guest IR' >&2; exit 1; }
for opt in -O0 -O2; do
    "$clang_binary" "$opt" -x ir "$tmp/trap.ll" -o "$tmp/trap"
    verify_compilation_unchanged "$tmp/trap.ll"
    native_status "$tmp/trap" trap
    verify_compilation_unchanged "$tmp/trap.ll"
done
compile_guest "$fixtures/pair-negative-bounds-trap.atto" "$tmp/pair-trap.ll"
grep -Fq 'icmp ult i64 1, ' "$tmp/pair-trap.ll" || {
    echo 'FAIL: bool/i64 guest omitted native bounds check for row 1' >&2; exit 1
}
for opt in -O0 -O2; do
    "$clang_binary" "$opt" -x ir "$tmp/pair-trap.ll" -o "$tmp/pair-trap"
    verify_compilation_unchanged "$tmp/pair-trap.ll"
    native_status "$tmp/pair-trap" trap
    verify_compilation_unchanged "$tmp/pair-trap.ll"
done
# Every unsupported source rejects without writing any IR: no partial codegen.
for item in \
  'negative-bool-as-int.atto|mixed inverted bool column' \
  'negative-f64-from-int.atto|mixed inverted f64 column' \
  'negative-immutable-bool-write.atto|immutable inverted field' \
  'negative-f64-return.atto|mixed inverted int main' \
  'negative-f64-infinite.atto|mixed inverted f64 literal' \
  'pair-negative-bool-as-int.atto|mixed inverted bool column' \
  'pair-negative-bool-to-int-return.atto|mixed inverted int main must return an i64 field' \
  'pair-negative-immutable-bool-set.atto|immutable inverted field' \
  'pair-negative-bool-write-int.atto|mixed inverted bool column' \
  'pair-negative-f64-unimplemented.atto|production inverted main requires two i64 fields'; do
    file="${item%%|*}"; diagnostic="${item#*|}"
    [[ -s "$fixtures/$file" && -f "$fixtures/$file" && ! -L "$fixtures/$file" ]] || {
        echo "FAIL: missing or symlinked negative source $file" >&2; exit 1
    }
    fixture_sha="$(hash_file "$fixtures/$file")"
    if "$compiler" "$fixtures/$file" >"$tmp/denied.ll" 2>"$tmp/denied.err"; then
        echo "FAIL: admitted negative mixed source $file" >&2
        exit 1
    fi
    if [[ -s "$tmp/denied.ll" ]] || ! grep -Fq "$diagnostic" "$tmp/denied.err"; then
        echo "FAIL: partial IR or wrong negative-source diagnostic: $file" >&2
        cat "$tmp/denied.err" >&2
        exit 1
    fi
    [[ "$(hash_file "$fixtures/$file")" == "$fixture_sha" ]] || {
        echo "FAIL: compiler modified rejected .atto source: $file" >&2; exit 1
    }
    require_tools_unchanged
done
[[ "$(hash_file "$compiler")" == "$compiler_sha" &&
   "$(hash_file "$auditor")" == "$auditor_sha" ]] ||
    { echo 'FAIL: compiler/auditor changed during native evidence run' >&2; exit 1; }
printf '%s\n' 'MIXED_ATTO_NATIVE_GUEST_PASS: 8 exit42, 4 trap signals, 10 rejects; IR no-GC screened (not linked/FFI proof)'
