#!/usr/bin/env bash
# Actual default oreslang-llvmc -> independently checked MIR -> LLVM IR ->
# Clang-linked native process. No experimental compiler flag is permitted.
set -euo pipefail
compiler="${1:?compiler}"
clang="${2:?clang}"
fixtures="${3:?fixtures}"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

for fixture in positive-column-get.ores positive-column-bounded.ores positive-production-erase-swap.ores; do
    "$compiler" "$fixtures/$fixture" > "$tmp/guest.ll"
    for required in '; typed MIR, experimental local i64 pair LLVM IR' \
        'load i1, ptr %alive' 'icmp ult i64' \
        'store volatile i1 false, ptr %alive' 'store volatile i64 0'; do
        if ! grep -Fq "$required" "$tmp/guest.ll"; then
            echo "FAIL: production source IR omits mandatory check: $required" >&2
            exit 1
        fi
    done
    "$clang" -O2 -x ir "$tmp/guest.ll" -o "$tmp/guest"
    set +e
    "$tmp/guest" >/dev/null 2>&1
    actual=$?
    set -e
    if [ "$actual" -ne 42 ]; then
        echo "FAIL: production native $fixture exited $actual, expected 42" >&2
        exit 1
    fi
done

# A guest read past logical length is dynamically guarded, rather than
# allowing an inbounds GEP from a capacity-only proof.
"$compiler" "$fixtures/negative-two-column-out-of-bounds.ores" > "$tmp/bounds.ll"
if ! grep -Fq 'call void @llvm.trap()' "$tmp/bounds.ll" ||
   ! grep -Fq 'icmp ult i64' "$tmp/bounds.ll"; then
    echo "FAIL: production bounds guest emitted no guarded trap" >&2
    exit 1
fi
"$clang" -O2 -x ir "$tmp/bounds.ll" -o "$tmp/bounds"
set +e
(ulimit -c 0; "$tmp/bounds" >/dev/null 2>&1) >/dev/null 2>&1
actual=$?
set -e
# llvm.trap must terminate by a trap signal (SIGILL or SIGTRAP),
# not any nonzero status such as a missing binary, loader failure or abort.
# On Bash/Linux these two signals produce 128+4 and 128+5.
if [ "$actual" -ne 132 ] && [ "$actual" -ne 133 ]; then
    echo "FAIL: production bounds guest did not terminate with SIGILL/SIGTRAP (exit $actual)" >&2
    exit 1
fi

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

if "$compiler" --profile=native-hard-rt "$fixtures/positive-column-get.ores" \
    >"$tmp/unsafe.ll" 2>"$tmp/error.txt"; then
    echo 'FAIL: unproved hard-RT claim admitted' >&2
    exit 1
fi
if [ -s "$tmp/unsafe.ll" ]; then
    echo 'FAIL: unproved hard-RT mode emitted IR' >&2
    exit 1
fi
printf 'PASS: production .ores -> typed MIR -> checked native IR, 3 exit-42, bounds trap, 6 rejects\n'
