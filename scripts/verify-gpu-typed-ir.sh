#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"
head="$(git rev-parse HEAD)"
tree="$(git rev-parse 'HEAD^{tree}')"

if [[ -n "${GPU_EXPECTED_HEAD_SHA:-}" && "${GPU_EXPECTED_HEAD_SHA}" != "$head" ]]; then
    echo "GPU_TYPED_IR_HOST_FAIL: checkout head mismatch" >&2
    exit 1
fi
echo "evidence_class=HOST_ONLY_TYPED_IR"
echo "compiler_guest_executed=false"
echo "physical_gpu_executed=false"
echo "source_head=$head"
echo "source_tree=$tree"

for path in \
    src/main/cpp/oreslang/runtime/GpuTypedLaunchIr.hpp \
    src/main/cpp/oreslang/runtime/GpuLaunchGeometry.hpp \
    tests/gpu_typed_launch_ir_test.cpp; do
    expected="$(git rev-parse "HEAD:$path")"
    observed="$(git hash-object "$path")"
    if [[ "$expected" != "$observed" ]]; then
        echo "GPU_TYPED_IR_HOST_FAIL: modified source blob $path" >&2
        exit 1
    fi
    echo "source_blob=$path:$observed"
done

temp_dir="$(mktemp -d)"
trap 'rm -rf "$temp_dir"' EXIT
compiler="${CXX:-c++}"
"$compiler" --version | head -n 1
"$compiler" -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror -UNDEBUG \
    -Isrc/main/cpp tests/gpu_typed_launch_ir_test.cpp -o "$temp_dir/typed-ir-test"
"$temp_dir/typed-ir-test"
echo "GPU_TYPED_IR_HOST_PASS: compiled exact checked-out blobs; no GPU dispatch"
