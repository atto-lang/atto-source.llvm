#!/usr/bin/env bash
# Exact-source Oreslang native enum AOT gate. This script must execute from
# the checked-out source Git tree; never replace it with synthetic model CI.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"
: "${CXX_CLANG:=clang++}"
: "${CXX_GCC:=g++}"
: "${CLANG:=clang}"
: "${PYTHON:=python3}"
: "${CMAKE_BUILD_TYPE:=Release}"
if ! command -v "$CXX_CLANG" >/dev/null 2>&1 ||
   ! command -v "$CXX_GCC" >/dev/null 2>&1 ||
   ! command -v "$CLANG" >/dev/null 2>&1 ||
   ! command -v "$PYTHON" >/dev/null 2>&1; then
    echo "ERROR: Clang, GCC and Python 3 required for exact-source enum AOT qualification" >&2
    exit 2
fi
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
head_sha="$(git rev-parse HEAD)"
tree_sha="$(git rev-parse HEAD^{tree})"
test -z "$(git status --porcelain)"
if [ -n "${EXPECTED_SOURCE_SHA:-}" ] &&
   [ "$head_sha" != "$EXPECTED_SOURCE_SHA" ]; then
    echo "ERROR: exact source commit mismatch; refusing uncertified build" >&2
    exit 3
fi
if [ -n "${EXPECTED_SOURCE_TREE:-}" ] &&
   [ "$tree_sha" != "$EXPECTED_SOURCE_TREE" ]; then
    echo "ERROR: exact source Git tree mismatch; refusing uncertified build" >&2
    exit 3
fi
echo "Exact source head: $head_sha"
echo "Exact source tree: $tree_sha"
echo "Tracked compiler/test blob IDs:"
git ls-tree -r HEAD -- \
    src/main/cpp/oreslang/frontend/NativeEnumAotSlice.hpp \
    src/main/cpp/oreslang/types/EnumCaseOverloadPolicy.hpp \
    src/main/cpp/oreslang/backend/LlvmCodegen.cpp \
    src/main/cpp/oreslang/main.cpp \
    tests/native_enum_aot_slice_test.cpp \
    tests/native_enum_guest_aot.py \
    tests/fixtures/enum-aot-basic.ores
echo "Clang: $("$CXX_CLANG" --version | head -1)"
echo "GCC: $("$CXX_GCC" --version | head -1)"

for compiler in "$CXX_CLANG" "$CXX_GCC"; do
    name="$("$compiler" --version | head -1 | cut -c 1-35 | tr ' /' '__')"
    "$compiler" -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror \
        -I src/main/cpp tests/native_enum_aot_slice_test.cpp \
        -o "$tmp/$name"
    "$tmp/$name"
done

# Use Clang's sanitizers for the real parser/selector header, not a stub.
"$CXX_CLANG" -std=c++17 -O1 -g -Wall -Wextra -Wpedantic -Werror \
    -fno-omit-frame-pointer -fsanitize=address,undefined \
    -I src/main/cpp tests/native_enum_aot_slice_test.cpp \
    -o "$tmp/enum-aot-asan-ubsan"
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1 "$tmp/enum-aot-asan-ubsan"

build="$tmp/build"
cmake -S . -B "$build" -DCMAKE_BUILD_TYPE="$CMAKE_BUILD_TYPE" \
    -DBUILD_TESTING=ON
cmake --build "$build" --target oreslang-llvmc oreslang-enum-source-aot-plan-test oreslang-enum-tag-boundary-test \
    --parallel 2
ctest --test-dir "$build" \
    -R 'native_enum_(source_aot_plan_integrity|tag_boundary|guest_aot_executable)' \
    --output-on-failure --timeout 120

"$PYTHON" tests/native_enum_guest_aot.py \
    --compiler "$(realpath "$build/oreslang-llvmc")" \
    --clang "$(command -v "$CLANG")" \
    --fixture tests/fixtures/enum-aot-basic.ores
echo "PASS: exact-source enum AOT native build and executed guest binary at $head_sha"
