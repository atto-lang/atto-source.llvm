#!/usr/bin/env bash
# Actual default oreslang-llvmc -> independently checked MIR -> LLVM IR ->
# Clang-linked native process. No experimental compiler flag is permitted.
set -euo pipefail
compiler="${1:?compiler}"
clang="${2:?clang}"
fixtures="${3:?fixtures}"
auditor="${4:?mandatory source-built no-GC IR auditor}"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
if [[ ! -f "$auditor" || ! -x "$auditor" || -L "$auditor" ||
      "${auditor##*/}" != "oreslang-no-gc-ir-audit" ]]; then
    echo "FAIL: missing or unsafe source-built no-GC IR auditor" >&2
    exit 1
fi
auditor_sha="$(sha256sum "$auditor" | cut -d' ' -f1)"
printf '%s\n' 'STRICT_NO_GC_IR_SCREENED_ONLY (linked binary and FFI audit still required)' \
    > "$tmp/expected-audit-success"
audit_ir() {
    local ir="$1"
    local before after
    if [[ ! -s "$ir" || "$ir" -ef "$auditor" ]]; then
        echo "FAIL: missing or auditor-aliased inverted guest IR" >&2
        exit 1
    fi
    before="$(sha256sum "$ir" | cut -d' ' -f1)"
    if ! "$auditor" "$ir" > "$tmp/audit.stdout" 2> "$tmp/audit.stderr"; then
        echo "FAIL: mandatory no-GC IR auditor rejected inverted guest" >&2
        cat "$tmp/audit.stderr" >&2
        exit 1
    fi
    after="$(sha256sum "$ir" | cut -d' ' -f1)"
    if ! cmp -s "$tmp/expected-audit-success" "$tmp/audit.stdout" ||
       [[ -s "$tmp/audit.stderr" || "$before" != "$after" ||
          "$(sha256sum "$auditor" | cut -d' ' -f1)" != "$auditor_sha" ]]; then
        echo "FAIL: forged, mutated or contaminated inverted no-GC IR audit" >&2
        exit 1
    fi
}
# The real auditor must deny a tracing-GC symbol before any native guest
# compilation; a stand-in that always prints SUCCESS cannot qualify parity.
printf '%s\n' 'declare ptr @GC_malloc(i64)' > "$tmp/known-gc.ll"
if "$auditor" "$tmp/known-gc.ll" > "$tmp/gc.stdout" 2> "$tmp/gc.stderr"; then
    echo "FAIL: no-GC IR auditor admitted known collector symbol" >&2
    exit 1
fi
if [[ -s "$tmp/gc.stdout" ]] ||
   ! grep -Fq 'STRICT_NO_GC_IR_REJECTED' "$tmp/gc.stderr"; then
    echo "FAIL: no-GC rejection lacked the compiler-owned diagnostic" >&2
    exit 1
fi
if [[ "$(sha256sum "$auditor" | cut -d' ' -f1)" != "$auditor_sha" ]]; then
    echo "FAIL: auditor mutated during known-GC negative" >&2
    exit 1
fi

for fixture in positive-column-get.atto positive-column-bounded.atto positive-production-erase-swap.atto; do
    "$compiler" "$fixtures/$fixture" > "$tmp/guest.ll"
    audit_ir "$tmp/guest.ll"
    for required in '; typed MIR, experimental local i64 pair LLVM IR' \
        'load i1, ptr %alive' 'icmp ult i64' \
        'store volatile i1 false, ptr %alive' 'store volatile i64 0'; do
        if ! grep -Fq "$required" "$tmp/guest.ll"; then
            echo "FAIL: production source IR omits mandatory check: $required" >&2
            exit 1
        fi
    done
    for optimization in -O0 -O2; do
        "$clang" "$optimization" -x ir "$tmp/guest.ll" -o "$tmp/guest"
        set +e
        "$tmp/guest" >/dev/null 2>&1
        actual=$?
        set -e
        if [ "$actual" -ne 42 ]; then
            echo "FAIL: production native $fixture $optimization exited $actual, expected 42" >&2
            exit 1
        fi
    done
done

# A guest read past logical length is dynamically guarded, rather than
# allowing an inbounds GEP from a capacity-only proof.
"$compiler" "$fixtures/negative-two-column-out-of-bounds.atto" > "$tmp/bounds.ll"
audit_ir "$tmp/bounds.ll"
if ! grep -Fq 'call void @llvm.trap()' "$tmp/bounds.ll" ||
   ! grep -Fq 'icmp ult i64' "$tmp/bounds.ll"; then
    echo "FAIL: production bounds guest emitted no guarded trap" >&2
    exit 1
fi
# Real signal evidence at both optimization levels; shell exit 132/133 can
# be forged by a normal return. Python checks SIGILL/SIGTRAP termination.
for optimization in -O0 -O2; do
    "$clang" "$optimization" -x ir "$tmp/bounds.ll" -o "$tmp/bounds"
    python3 - "$tmp/bounds" <<'PY'
import resource
import signal
import subprocess
import sys

resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
try:
    result = subprocess.run(
        [sys.argv[1]], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        timeout=15, check=False
    )
except (OSError, subprocess.TimeoutExpired) as exc:
    raise SystemExit(f"FAIL: bounds guest did not execute: {exc}")
if result.returncode not in (-signal.SIGILL, -signal.SIGTRAP):
    raise SystemExit(
        f"FAIL: bounds guest expected a real SIGILL/SIGTRAP, got {result.returncode}"
    )
PY
done

# All unsupported source must reject before emitting a single byte of IR.
cat > "$tmp/immutable.atto" <<'ORES'
define inverted struct P as let x: int; id: int; end
fnc main(): int {
    let rows = InvertedArray<P>.with_capacity(2);
    rows.push(11, 22);
    rows.set(0, id, 42);
    return rows.get(0, x);
}
ORES
cat > "$tmp/foreign.atto" <<'ORES'
define inverted struct P as let x: int; id: int; end
fnc main(): int {
    let rows = InvertedArray<P>.with_capacity(2);
    other.push(11, 22);
    return rows.get(0, x);
}
ORES
cat > "$tmp/erased.atto" <<'ORES'
define inverted struct P as let x: int; id: int; end
fnc main(): int {
    let rows = InvertedArray<P>.with_capacity(2);
    rows.push(11, 22);
    rows.erase_swap(1);
    return rows.get(0, x);
}
ORES
cat > "$tmp/mixed.atto" <<'ORES'
define inverted struct P as let x: int; id: bool; end
fnc main(): int {
    let rows = InvertedArray<P>.with_capacity(2);
    rows.push(11, 22);
    return rows.get(0, x);
}
ORES
cat > "$tmp/extra.atto" <<'ORES'
define inverted struct P as let x: int; id: int; end
fnc main(): int {
    let rows = InvertedArray<P>.with_capacity(2);
    rows.push(11, 22);
    return rows.get(0, x);
}
fnc hidden(): int { return 7; }
ORES
for item in \
    'immutable|immutable inverted field' \
    'foreign|must target the local owner' \
    'erased|outside current length' \
    'mixed|precisely two i64 fields' \
    'extra|only source callable'; do
    name="${item%%|*}"; diagnostic="${item#*|}"
    if "$compiler" "$tmp/$name.atto" > "$tmp/unsafe.ll" 2> "$tmp/error.txt"; then
        echo "FAIL: production compiler admitted unsafe $name" >&2
        exit 1
    fi
    if [ -s "$tmp/unsafe.ll" ] || ! grep -Fq "$diagnostic" "$tmp/error.txt"; then
        echo "FAIL: wrong rejection or emitted IR for $name" >&2
        cat "$tmp/error.txt" >&2
        exit 1
    fi
done

# Unsupported runtime/target/analysis options must fail closed before any
# LLVM IR bytes escape. Do not grant Wasm, hard-RT or allocator claims.
for option in --profile=native-hard-rt --target=wasm32 \
              --constraint-descriptors --tree-shake \
              --allocator-provider=bounded-arena; do
    if "$compiler" "$option" "$fixtures/positive-column-get.atto" \
        >"$tmp/unsafe.ll" 2>"$tmp/error.txt"; then
        echo "FAIL: restricted production owner admitted option $option" >&2
        exit 1
    fi
    if [ -s "$tmp/unsafe.ll" ]; then
        echo "FAIL: restricted owner emitted IR for unsupported option $option" >&2
        exit 1
    fi
done
printf 'PASS: production .atto -> typed MIR -> no-GC-screened IR, 6 native exit-42 (O0/O2), 2 bounds signals, 5 source rejects, 5 mode rejects, known-GC denied (linked/FFI audit still unproven)\n'
