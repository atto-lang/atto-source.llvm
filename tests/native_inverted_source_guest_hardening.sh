#!/usr/bin/env bash
# Production .atto -> oreslang-llvmc -> LLVM IR -> Clang native process.
# Only a restricted stack-local two-i64 inverted struct is admitted.
set -euo pipefail
compiler="$1"
clang="$2"
fixtures="$3"
test -f "$compiler" && test -x "$compiler" && test ! -L "$compiler"
test -d "$fixtures"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

compile_ir() {
    local name="$1" source="$2"
    "$compiler" "$source" > "$tmp/$name.ll" 2>"$tmp/$name.compile.err"
    test -s "$tmp/$name.ll"
    grep -Fq 'define i32 @main()' "$tmp/$name.ll"
    grep -Fq 'declare void @llvm.trap()' "$tmp/$name.ll"
    grep -Fq 'store volatile i1 false, ptr %alive' "$tmp/$name.ll"
    if grep -Eqi 'GC_malloc|GC_collect|Boehm|GC_init' "$tmp/$name.ll"; then
        echo "FAIL: $name emitted tracing GC symbol" >&2
        exit 1
    fi
}
compile_ir swap-reuse "$fixtures/swap-reuse.atto"
compile_ir removed-index-trap "$fixtures/removed-index-trap.atto"

for opt in 0 2; do
    for name in swap-reuse removed-index-trap; do
        "$clang" -O"$opt" -x ir "$tmp/$name.ll" -o "$tmp/$name-$opt"
        set +e
        (ulimit -c 0; timeout 15s "$tmp/$name-$opt" >/dev/null 2>&1) 2>/dev/null
        result=$?
        set -e
        if [[ "$name" == "swap-reuse" ]]; then
            if [[ "$result" != 42 ]]; then
                echo "FAIL: real .atto $name -O$opt exited $result, expected 42" >&2
                exit 1
            fi
            echo "ATTO_INVERTED_NATIVE_EXIT_42_O$opt"
        else
            if [[ "$result" != 132 && "$result" != 133 ]]; then
                echo "FAIL: real .atto $name -O$opt exited $result, expected SIGILL/SIGTRAP" >&2
                exit 1
            fi
            echo "ATTO_INVERTED_NATIVE_BOUNDS_TRAP_O$opt"
        fi
    done
done

expect_reject() {
    local name="$1" expected="$2" source="$3"
    printf '%s\n' "$source" > "$tmp/$name.atto"
    set +e
    "$compiler" "$tmp/$name.atto" >"$tmp/$name.stdout" 2>"$tmp/$name.stderr"
    local status=$?
    set -e
    if [[ "$status" == 0 || -s "$tmp/$name.stdout" ]] ||
            ! grep -Fq "$expected" "$tmp/$name.stderr"; then
        echo "FAIL: $name expected source diagnostic: $expected (exit=$status)" >&2
        cat "$tmp/$name.stderr" >&2
        exit 1
    fi
    echo "ATTO_INVERTED_REJECT_$name"
}

expect_reject inverted_class 'DOP_INVERTED_CLASS_UNSUPPORTED' \
'define inverted class Entity as
    let value: int;
end
fnc main(): int { return 42; }'

expect_reject unsupported_bool_schema 'production inverted main requires precisely two i64 fields' \
'define inverted struct Row as
    let value: bool;
    token: int;
end
fnc main(): int {
    let rows = InvertedArray<Row>.with_capacity(3);
    rows.push(7, 70);
    return rows.get(0, value);
}'

expect_reject duplicate_field 'duplicate inverted struct field' \
'define inverted struct Row as
    let value: int;
    let value: int;
end
fnc main(): int { return 42; }'

expect_reject immutable_write 'immutable inverted field cannot be written' \
'define inverted struct Row as
    let value: int;
    token: int;
end
fnc main(): int {
    let rows = InvertedArray<Row>.with_capacity(3);
    rows.push(7, 70);
    rows.set(0, token, 88);
    return rows.get(0, value);
}'

expect_reject unknown_field 'unknown inverted field' \
'define inverted struct Row as
    let value: int;
    token: int;
end
fnc main(): int {
    let rows = InvertedArray<Row>.with_capacity(3);
    rows.push(7, 70);
    return rows.get(0, missing);
}'

expect_reject capacity_overflow 'production inverted capacity must be 1..256' \
'define inverted struct Row as
    let value: int;
    token: int;
end
fnc main(): int {
    let rows = InvertedArray<Row>.with_capacity(257);
    rows.push(7, 70);
    return rows.get(0, value);
}'

expect_reject wrong_owner 'production inverted operation must target the local owner' \
'define inverted struct Row as
    let value: int;
    token: int;
end
fnc main(): int {
    let rows = InvertedArray<Row>.with_capacity(3);
    other.push(7, 70);
    return rows.get(0, value);
}'

expect_reject negative_index 'production inverted row index must fit nonnegative size_t' \
'define inverted struct Row as
    let value: int;
    token: int;
end
fnc main(): int {
    let rows = InvertedArray<Row>.with_capacity(3);
    rows.push(7, 70);
    return rows.get(-1, value);
}'

expect_reject extra_function 'production inverted main must be the only source callable' \
'define inverted struct Row as
    let value: int;
    token: int;
end
fnc main(): int {
    let rows = InvertedArray<Row>.with_capacity(3);
    rows.push(7, 70);
    return rows.get(0, value);
}
fnc other(): int { return 1; }'

expect_reject pointer_member 'inverted struct member requires fixed scalar' \
'define inverted struct Row as
    let value: Pointer<int>;
    token: int;
end
fnc main(): int { return 42; }'

echo 'REAL_ATTO_INVERTED_SOURCE_GUEST_PASS; scope=two_i64_local_main_only; full_repo_CTest=NOT_EXECUTED'
