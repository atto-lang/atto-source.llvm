#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

head="$(git rev-parse HEAD)"
tree="$(git rev-parse HEAD^{tree})"
if [[ -n "${ATTO_EXPECTED_HEAD_SHA:-}" && "$head" != "$ATTO_EXPECTED_HEAD_SHA" ]]; then
  echo "GPU_G2_FAIL: expected PR source head mismatch" >&2
  exit 1
fi

paths=(
  'tools/atto_gpu_saxpy_kernelc.cpp'
  'tests/gpu-saxpy-g2/atto-gpu-saxpy-kernelc.test.mjs'
  'tests/gpu-saxpy-g2/saxpy.atto'
  'scripts/verify-atto-gpu-saxpy-g2.sh'
)
for path in "${paths[@]}"; do
  expected="$(git rev-parse "HEAD:$path")"
  observed="$(git hash-object "$path")"
  if [[ "$expected" != "$observed" ]]; then
    echo "GPU_G2_FAIL: tested file changed from immutable head: $path" >&2
    exit 1
  fi
  printf 'source_blob %s %s\n' "$path" "$observed"
done

compiler="${CXX:-clang++}"
opencl_compiler="${CLANG_OPENCL:-clang}"
command -v "$compiler" >/dev/null || { echo 'GPU_G2_FAIL: C++ compiler missing' >&2; exit 1; }
command -v "$opencl_compiler" >/dev/null || { echo 'GPU_G2_FAIL: OpenCL frontend missing' >&2; exit 1; }
command -v node >/dev/null || { echo 'GPU_G2_FAIL: Node.js test runner missing' >&2; exit 1; }

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
printf 'source_head %s\nsource_tree %s\n' "$head" "$tree"
printf 'evidence_class COMPILER_DEVICE_SOURCE_ONLY\nguest_host_launch_executed false\nphysical_gpu_executed false\n'
"$compiler" --version | head -n1
"$opencl_compiler" --version | head -n1
node --version
"$compiler" -std=c++17 -Wall -Wextra -Wpedantic -Werror \
  tools/atto_gpu_saxpy_kernelc.cpp -o "$work/atto-gpu-saxpy-kernelc"
ATTO_GPU_KERNELC="$work/atto-gpu-saxpy-kernelc" \
ATTO_GPU_CLANG="$opencl_compiler" \
  node --test tests/gpu-saxpy-g2/atto-gpu-saxpy-kernelc.test.mjs
"$work/atto-gpu-saxpy-kernelc" tests/gpu-saxpy-g2/saxpy.atto "$work/kernel.cl"
"$opencl_compiler" -x cl -cl-std=CL1.2 -fsyntax-only "$work/kernel.cl"
"$opencl_compiler" -target spir64-unknown-unknown -x cl -cl-std=CL1.2 -S -emit-llvm -o "$work/kernel.spir64.ll" "$work/kernel.cl"
grep -q 'target triple = "spir64-unknown-unknown"' "$work/kernel.spir64.ll"
grep -q 'spir_kernel void @attolang_saxpy_v1' "$work/kernel.spir64.ll"
printf 'target_ir_class SPIR64_LLVM_IR_ONLY\n'
printf 'device_artifact_sha256 %s\n' "$(sha256sum "$work/kernel.cl" | cut -d' ' -f1)"
printf 'spir64_ir_sha256 %s\n' "$(sha256sum "$work/kernel.spir64.ll" | cut -d' ' -f1)"
printf 'GPU_G2_PASS: bounded .atto device source compiled; NO physical GPU and NO host launch executed\n'
