#!/usr/bin/env bash
# Actual default oreslang-llvmc -> independently checked MIR -> LLVM IR ->
# Clang-linked native process. No experimental compiler flag is permitted.
set -euo pipefail
compiler="${1:?compiler}"
clang="${2:?clang}"
fixtures="${3:?fixtures}"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

for fixture in positive-column-get.ores positive-column-bounded.ores positive-production-erase-swap.ores \
    positive-production-snapshot.atto positive-production-snapshot-id.atto \
    positive-production-snapshot-ordinal.atto; do
    "$compiler" "$fixtures/$fixture" > "$tmp/guest.ll"
    for required in '; typed MIR, experimental local i64 pair LLVM IR' \
        'load i1, ptr %alive' 'icmp ult i64' \
        'store volatile i1 false, ptr %alive' 'store volatile i64 0'; do
        if ! grep -Fq "$required" "$tmp/guest.ll"; then
            echo "FAIL: production source IR omits mandatory check: $required" >&2
            exit 1
        fi
    done
    for opt in -O0 -O2; do
        "$clang" "$opt" -x ir "$tmp/guest.ll" -o "$tmp/guest"
        set +e
        "$tmp/guest" >/dev/null 2>&1
        actual=$?
        set -e
        if [ "$actual" -ne 42 ]; then
            echo "FAIL: production native $fixture $opt exited $actual, expected 42" >&2
            exit 1
        fi
    done
done

# A guest read past logical length is dynamically guarded, rather than
# allowing an inbounds GEP from a capacity-only proof.
for bad in negative-two-column-out-of-bounds.ores negative-production-snapshot-bounds.atto; do
 "$compiler" "$fixtures/$bad" > "$tmp/bounds.ll"
 if ! grep -Fq 'call void @llvm.trap()' "$tmp/bounds.ll" ||
    ! grep -Fq 'icmp ult i64' "$tmp/bounds.ll"; then
    echo "FAIL: production $bad emitted no guarded trap" >&2
    exit 1
 fi
 for opt in -O0 -O2; do
  "$clang" "$opt" -x ir "$tmp/bounds.ll" -o "$tmp/bounds"
# A shell status of 132/133 is ambiguous: a program can return that exit code
# without receiving SIGILL/SIGTRAP. Python retains the actual signal number.
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
done

# All unsupported source must reject before emitting a single byte of IR.
cat > "$tmp/immutable.ores" <<'ORES'
define inverted struct P as let x: int; id: int; end
fnc main(): int {
    let rows = InvertedArray<P>.with_capacity(2);
    rows.push(11, 22);
    rows.set(0, id, 42);
    return rows.get(0, x);
}
ORES
cat > "$tmp/foreign.ores" <<'ORES'
define inverted struct P as let x: int; id: int; end
fnc main(): int {
    let rows = InvertedArray<P>.with_capacity(2);
    other.push(11, 22);
    return rows.get(0, x);
}
ORES
cat > "$tmp/erased.ores" <<'ORES'
define inverted struct P as let x: int; id: int; end
fnc main(): int {
    let rows = InvertedArray<P>.with_capacity(2);
    rows.push(11, 22);
    rows.erase_swap(1);
    return rows.get(0, x);
}
ORES
cat > "$tmp/mixed.ores" <<'ORES'
define inverted struct P as let x: int; id: bool; end
fnc main(): int {
    let rows = InvertedArray<P>.with_capacity(2);
    rows.push(11, 22);
    return rows.get(0, x);
}
ORES
cat > "$tmp/extra.ores" <<'ORES'
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
    if "$compiler" "$tmp/$name.ores" > "$tmp/unsafe.ll" 2> "$tmp/error.txt"; then
        echo "FAIL: production compiler admitted unsafe $name" >&2
        exit 1
    fi
    if [ -s "$tmp/unsafe.ll" ] || ! grep -Fq "$diagnostic" "$tmp/error.txt"; then
        echo "FAIL: wrong rejection or emitted IR for $name" >&2
        cat "$tmp/error.txt" >&2
        exit 1
    fi
done

cat > "$tmp/unknown-snapshot.atto" <<'ATTO'
define inverted struct P as let x: int; id: int; end
fnc main(): int {
    let rows = InvertedArray<P>.with_capacity(2);
    rows.push(42, 7);
    return missing.x;
}
ATTO
cat > "$tmp/duplicate-snapshot.atto" <<'ATTO'
define inverted struct P as let x: int; id: int; end
fnc main(): int {
    let rows = InvertedArray<P>.with_capacity(2);
    rows.push(42, 7);
    let saved = rows.snapshot(0);
    let saved = rows.snapshot(0);
    return saved.x;
}
ATTO
cat > "$tmp/foreign-snapshot.atto" <<'ATTO'
define inverted struct P as let x: int; id: int; end
fnc main(): int {
    let rows = InvertedArray<P>.with_capacity(2);
    rows.push(42, 7);
    let saved = other.snapshot(0);
    return saved.x;
}
ATTO
for item in \
    'unknown-snapshot|return requires declared snapshot' \
    'duplicate-snapshot|snapshot binding must be distinct' \
    'foreign-snapshot|snapshot must target the local owner'; do
    name="${item%%|*}"; diagnostic="${item#*|}"
    if "$compiler" "$tmp/$name.atto" > "$tmp/unsafe.ll" 2> "$tmp/error.txt"; then
        echo "FAIL: production compiler admitted unsafe $name" >&2; exit 1
    fi
    if [ -s "$tmp/unsafe.ll" ] || ! grep -Fq "$diagnostic" "$tmp/error.txt"; then
        echo "FAIL: wrong snapshot rejection or emitted IR for $name" >&2
        cat "$tmp/error.txt" >&2
        exit 1
    fi
done

if "$compiler" --profile=native-hard-rt "$fixtures/positive-column-get.ores" \
    >"$tmp/unsafe.ll" 2>"$tmp/error.txt"; then
    echo 'FAIL: unproved hard-RT claim admitted' >&2
    exit 1
fi
if [ -s "$tmp/unsafe.ll" ]; then
    echo 'FAIL: unproved hard-RT mode emitted IR' >&2
    exit 1
fi
printf 'PASS: production .ores/.atto -> typed MIR -> checked native IR, 6 exit-42 (O0/O2), 2 bounds traps (O0/O2), 9 rejects\n'
