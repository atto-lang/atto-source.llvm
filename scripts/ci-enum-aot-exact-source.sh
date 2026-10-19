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
   ! command -v "$PYTHON" >/dev/null 2>&1 ||
   ! command -v cmake >/dev/null 2>&1 ||
   ! command -v ctest >/dev/null 2>&1 ||
   ! command -v git >/dev/null 2>&1; then
    echo "ERROR: Git, CMake, CTest, Clang, GCC and Python 3 are required for exact-source enum AOT qualification" >&2
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
# Build every host test whose result we intend to call release evidence.
cmake --build "$build" --parallel 2 --target \
    oreslang-llvmc \
    oreslang-native-enum-case-overload-policy-test \
    oreslang-enum-tag32-layout-test \
    oreslang-enum-case-aot-link-test \
    oreslang-enum-tag-boundary-test \
    oreslang-enum-source-aot-plan-test

# Prevent vacuous green CI: CTest normally exits successfully when -R
# matches zero tests. Require the complete bounded enum AOT test inventory.
ctest --test-dir "$build" --show-only=json-v1 > "$tmp/ctest-inventory.json"
"$PYTHON" - "$tmp/ctest-inventory.json" <<'PY'
import json
import sys
from pathlib import Path

required = {
    "native_enum_case_overload_static_policy",
    "native_enum_case_exhaustive_relation_python",
    "native_enum_case_exhaustive_relation_python_O",
    "native_enum_case_eos2_abi_golden_python",
    "native_enum_case_eos2_abi_golden_python_O",
    "native_enum_case_nullary_tag32_schema_python",
    "native_enum_case_nullary_tag32_schema_python_O",
    "native_enum_tag32_aot_layout",
    "native_enum_case_eos2_closed_aot_link",
    "native_enum_tag_boundary",
    "native_enum_source_aot_plan_integrity",
    "native_enum_guest_aot_executable",
}
registered = {item["name"] for item in json.loads(Path(sys.argv[1]).read_text())["tests"]}
missing = sorted(required - registered)
if missing:
    sys.exit("ERROR: required exact-source enum AOT CTests absent: " + ", ".join(missing))
print(f"PASS: all {len(required)} required enum AOT tests registered")
PY

ctest --test-dir "$build" \
    -R '^native_enum_' --output-on-failure --timeout 120

"$PYTHON" tests/native_enum_guest_aot.py \
    --compiler "$(realpath "$build/oreslang-llvmc")" \
    --clang "$(command -v "$CLANG")" \
    --fixture tests/fixtures/enum-aot-basic.ores
echo "PASS: exact-source enum AOT native build and executed guest binary at $head_sha"
