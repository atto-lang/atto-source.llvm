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
command -v python3 >/dev/null 2>&1 || { echo 'FAIL: Python3 required for bounded native executions' >&2; exit 1; }
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
compiler_sha="$(hash_file "$compiler")" || exit 1
auditor_sha="$(hash_file "$auditor")" || exit 1
printf '%s\n' 'declare ptr @GC_malloc(i64)' > "$tmp/known-gc.ll"
if "$auditor" "$tmp/known-gc.ll" >"$tmp/known-gc.out" 2>"$tmp/known-gc.err"; then
    echo 'FAIL: source-built auditor admitted collector import' >&2
    exit 1
fi
grep -Fq 'STRICT_NO_GC_IR_REJECTED' "$tmp/known-gc.err" ||
    { echo 'FAIL: missing known-GC diagnostic' >&2; exit 1; }

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
    if ! grep -Fxq 'STRICT_NO_GC_IR_SCREENED_ONLY (linked binary and FFI audit still required)' "$tmp/audit.out" ||
       [[ -s "$tmp/audit.err" || "$before" != "$after" ||
          "$(hash_file "$auditor")" != "$auditor_sha" ]]; then
        echo 'FAIL: mixed source IR audit or provenance changed' >&2
        exit 1
    fi
}
# Verify a *real* native process and bound its execution independently of CTest.
native_status() {
    python3 - "$1" "$2" <<'PY'
import resource
import subprocess
import sys
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
try:
    run = subprocess.run([sys.argv[1]], stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL, timeout=15, check=False)
except (OSError, subprocess.TimeoutExpired) as exc:
    raise SystemExit(f"FAIL: native mixed guest did not complete: {exc}")
if sys.argv[2] != "exit42" or run.returncode != 42:
    raise SystemExit(f"FAIL: native mixed guest expected exit 42 got {run.returncode}")
PY
}
for file in positive-mixed-swap.atto positive-permuted-columns.atto; do
    "$compiler" "$fixtures/$file" > "$tmp/guest.ll"
    audit_ir "$tmp/guest.ll"
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
    for opt in -O0 -O2; do
        "$clang" "$opt" -x ir "$tmp/guest.ll" -o "$tmp/guest"
        native_status "$tmp/guest" exit42
    done
done
# Stale dense indices are statically known here: the HIR proves the row was
# deleted and must reject before LLVM IR. A native trap would be unreachable
# without adding runtime-dependent indices to this intentionally literal-only
# source grammar. Preserve this stronger compile-time rule; the positive IR
# must still contain the actual checked LLVM trap branch for runtime use.
# Every unsupported source rejects without writing any IR: no partial codegen.
for item in \
  'negative-stale-dense-index.atto|inverted row index outside current length' \
  'negative-bool-as-int.atto|mixed inverted bool column' \
  'negative-f64-from-int.atto|mixed inverted f64 column' \
  'negative-immutable-bool-write.atto|immutable inverted field' \
  'negative-f64-return.atto|mixed inverted int main' \
  'negative-f64-infinite.atto|mixed inverted f64 literal'; do
    file="${item%%|*}"; diagnostic="${item#*|}"
    if "$compiler" "$fixtures/$file" >"$tmp/denied.ll" 2>"$tmp/denied.err"; then
        echo "FAIL: admitted negative mixed source $file" >&2
        exit 1
    fi
    if [[ -s "$tmp/denied.ll" ]] || ! grep -Fq "$diagnostic" "$tmp/denied.err"; then
        echo "FAIL: partial IR or wrong negative-source diagnostic: $file" >&2
        cat "$tmp/denied.err" >&2
        exit 1
    fi
done
[[ "$(hash_file "$compiler")" == "$compiler_sha" &&
   "$(hash_file "$auditor")" == "$auditor_sha" ]] ||
    { echo 'FAIL: compiler/auditor changed during native evidence run' >&2; exit 1; }
printf '%s\n' 'MIXED_ATTO_NATIVE_GUEST_PASS: 4 exit42, 6 compile rejects, source IR trap guards present; IR no-GC screened (not linked/FFI proof)'
