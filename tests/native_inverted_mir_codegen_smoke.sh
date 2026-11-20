#!/usr/bin/env bash
# Source-local narrow MIR/AOT validation. NOT a production .atto guest test.
# No Python, external runner secrets, or experimental language switches.
set -euo pipefail
repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
gxx="${CXX_GCC:-g++}"
clangxx="${CXX_CLANG:-clang++}"
clang="${CLANG:-clang}"

"$gxx" -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror -UNDEBUG \
  -I"$repo/src/main/cpp" \
  "$repo/tests/native_inverted_production_mir_test.cpp" -o "$tmp/mir-gcc"
"$tmp/mir-gcc"

"$clangxx" -std=c++17 -O1 -g -Wall -Wextra -Wpedantic -Werror -UNDEBUG \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -I"$repo/src/main/cpp" \
  "$repo/tests/native_inverted_production_mir_test.cpp" -o "$tmp/mir-clang"
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$tmp/mir-clang"

"$gxx" -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror -UNDEBUG \
  -I"$repo/src/main/cpp" \
  "$repo/tests/native_inverted_mir_codegen_smoke.cpp" -o "$tmp/mir-emitter"
"$tmp/mir-emitter" > "$tmp/guest.ll"
"$tmp/mir-emitter" --bounds-trap > "$tmp/negative.ll"
test -s "$tmp/guest.ll"
test -s "$tmp/negative.ll"
grep -Fq 'define i32 @main()' "$tmp/guest.ll"
grep -Fq 'llvm.trap' "$tmp/guest.ll"
if grep -Eqi 'GC_malloc|GC_collect|Boehm' "$tmp/guest.ll"; then
    echo 'FAIL: generated IR references tracing GC' >&2
    exit 1
fi
for opt in 0 2; do
    "$clang" -O"$opt" -x ir "$tmp/guest.ll" -o "$tmp/guest-$opt"
    set +e
    "$tmp/guest-$opt"
    result=$?
    set -e
    if [[ "$result" != 42 ]]; then
        echo "FAIL: native checked MIR artifact O$opt exited $result, expected 42" >&2
        exit 1
    fi
    echo "MIR_IR_NATIVE_EXIT_42_O$opt"
    "$clang" -O"$opt" -x ir "$tmp/negative.ll" -o "$tmp/negative-$opt"
    # LLVM llvm.trap lowers to an actual illegal-instruction or trap signal.
    # A normal exit 42, graceful zero exit, SIGSEGV or SIGABRT is not proof.
    (ulimit -c 0; "$tmp/negative-$opt" >/dev/null 2>&1) 2>/dev/null && {
        echo "FAIL: checked dense out-of-bounds returned normally at O$opt" >&2
        exit 1
    }
    set +e
    (ulimit -c 0; "$tmp/negative-$opt" >/dev/null 2>&1) 2>/dev/null
    trapped=$?
    set -e
    if [[ "$trapped" != 132 && "$trapped" != 133 ]]; then
        echo "FAIL: expected SIGILL/SIGTRAP, got exit $trapped at O$opt" >&2
        exit 1
    fi
    echo "MIR_IR_BOUNDS_TRAP_O$opt"
done

# Additional deterministic physical-column cases. Every input goes through
# verified compiler-constructed HIR/MIR (not the .atto parser), real LLVM IR
# emission, and a fresh Clang native executable at each optimization level.
for case_name in bulk-reuse tail-delete overwrite; do
    "$tmp/mir-emitter" "--$case_name" > "$tmp/$case_name.ll"
    test -s "$tmp/$case_name.ll"
    grep -Fq 'declare void @llvm.trap()' "$tmp/$case_name.ll"
    for opt in 0 2; do
        "$clang" -O"$opt" -x ir "$tmp/$case_name.ll" -o "$tmp/$case_name-$opt"
        set +e
        "$tmp/$case_name-$opt"
        result=$?
        set -e
        if [[ "$result" != 42 ]]; then
            echo "FAIL: $case_name native AOT -O$opt exited $result, expected 42" >&2
            exit 1
        fi
        echo "MIR_IR_NATIVE_$case_name-EXIT_42_O$opt"
    done
done
echo 'CHECKED_MIR_NATIVE_IR_PASS; .atto source parser NOT_EXECUTED'
