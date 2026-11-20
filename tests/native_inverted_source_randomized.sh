#!/usr/bin/env bash
# Deterministic dense-index differential oracle using real .atto guest compiler.
# This does NOT test generational RowHandle object identity.
set -euo pipefail
compiler="$1"
clang="$2"
test -x "$compiler" && test -f "$compiler" && test ! -L "$compiler"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

for original in 7 19 31 43 59 83; do
    seed="$original"
    n=0
    declare -a values=()
    file="$tmp/rows-$original.atto"
    printf 'define inverted struct Row as\n    let value: int;\n    token: int;\nend\nfnc main(): int {\n    let rows = InvertedArray<Row>.with_capacity(18);\n' > "$file"
    for ((step=0; step<48; ++step)); do
        seed=$(( (seed*1103515245 + 12345) & 2147483647 ))
        mode=$((seed%3))
        if ((n==0)); then mode=0; fi
        if ((n==18 && mode==0)); then mode=2; fi
        if ((mode==0)); then
            seed=$(( (seed*1103515245 + 12345) & 2147483647 ))
            value=$((seed%125 + 1))
            printf '    rows.push(%s, %s);\n' "$value" "$((step*10+original))" >> "$file"
            values[n]="$value"
            n=$((n+1))
        elif ((mode==1)); then
            index=$((seed%n))
            seed=$(( (seed*1103515245 + 12345) & 2147483647 ))
            value=$((seed%125+1))
            printf '    rows.set(%s, value, %s);\n' "$index" "$value" >> "$file"
            values[index]="$value"
        else
            index=$((seed%n))
            printf '    rows.erase_swap(%s);\n' "$index" >> "$file"
            last=$((n-1))
            values[index]="$((values[last]))"
            unset 'values[last]'
            n=$((n-1))
        fi
    done
    if ((n==0)); then
        printf '    rows.push(42, 1);\n' >> "$file"
        values[0]=42
        n=1
    fi
    seed=$(( (seed*1103515245 + 12345) & 2147483647 ))
    index=$((seed%n))
    expected=$((values[index]))
    printf '    return rows.get(%s, value);\n}\n' "$index" >> "$file"
    "$compiler" "$file" > "$tmp/$original.ll" 2> "$tmp/$original.err"
    test -s "$tmp/$original.ll"
    grep -Fq 'declare void @llvm.trap()' "$tmp/$original.ll"
    grep -Fq 'store volatile i1 false, ptr %alive' "$tmp/$original.ll"
    if grep -Eqi 'GC_malloc|GC_collect|Boehm|GC_init' "$tmp/$original.ll"; then exit 1; fi
    for opt in 0 2; do
        "$clang" -O"$opt" -x ir "$tmp/$original.ll" -o "$tmp/$original-$opt"
        set +e
        timeout 15s "$tmp/$original-$opt" >/dev/null 2>&1
        result=$?
        set -e
        if [[ "$result" != "$expected" ]]; then
            echo "FAIL: seed=$original O$opt got=$result oracle=$expected" >&2
            exit 1
        fi
        echo "ATTO_RANDOM_SEED_$original-OPT_$opt-EXPECTED_$expected-PASS"
    done
    unset values
done

cat > "$tmp/overcapacity.atto" <<'ATTO'
define inverted struct Row as
    let value: int;
    token: int;
end
fnc main(): int {
    let rows = InvertedArray<Row>.with_capacity(1);
    rows.push(1, 2);
    rows.push(3, 4);
    return rows.get(0, value);
}
ATTO
set +e
"$compiler" "$tmp/overcapacity.atto" > "$tmp/overcapacity.ir" 2> "$tmp/overcapacity.err"
result=$?
set -e
if [[ "$result" == 0 || -s "$tmp/overcapacity.ir" ]] ||
        ! grep -Fq 'inverted collection capacity exceeded' "$tmp/overcapacity.err"; then
    echo 'FAIL: overcapacity source not rejected' >&2
    cat "$tmp/overcapacity.err" >&2
    exit 1
fi
echo 'ATTO_RANDOMIZED_CAPACITY_REJECT_PASS'
echo 'ATTO_RANDOMIZED_SOURCE_GUEST_PASS; seeds=6; operations=48; O0+O2'
