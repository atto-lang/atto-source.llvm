#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cxx="${CXX:-c++}"
clang="${GPU_OPENCL_CLANG:-clang}"
command -v "$cxx" >/dev/null || { echo "GPU_OPENCL_OFFLINE_FAIL: no C++ compiler" >&2; exit 1; }
command -v "$clang" >/dev/null || { echo "GPU_OPENCL_OFFLINE_FAIL: no clang OpenCL compiler" >&2; exit 1; }
"$cxx" -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror -UNDEBUG \
  -Isrc/main/cpp tests/gpu_kernel_opencl_source_test.cpp -o "$work/offline-source-check"
"$work/offline-source-check" "$work/saxpy.cl"
"$clang" -x cl -cl-std=CL1.2 -fsyntax-only "$work/saxpy.cl"
"$clang" --target=spir64-unknown-unknown -x cl -cl-std=CL1.2 \
  -S -emit-llvm "$work/saxpy.cl" -o "$work/saxpy-spir64.ll"
grep -Eq 'target triple = "spir64-' "$work/saxpy-spir64.ll"
grep -Eq 'define .*spir_kernel .*@atto_saxpy_v0' "$work/saxpy-spir64.ll"
grep -Eq 'get_global_id' "$work/saxpy-spir64.ll"
grep -Eq 'fmul float' "$work/saxpy-spir64.ll"
grep -Eq 'fadd float' "$work/saxpy-spir64.ll"
grep -Eq 'load float' "$work/saxpy-spir64.ll"
grep -Eq 'store float' "$work/saxpy-spir64.ll"
echo "evidence_class=OPENCL_SOURCE_SPIR64_IR_OFFLINE_ONLY"
echo "opencl_source_sha256=$(sha256sum "$work/saxpy.cl" | cut -d' ' -f1)"
echo "spir64_llvm_ir_sha256=$(sha256sum "$work/saxpy-spir64.ll" | cut -d' ' -f1)"
echo "real_attolang_llvmc_executed=false"
echo "device_binary_executed=false"
echo "physical_gpu_executed=false"
echo "GPU_OPENCL_OFFLINE_PASS: source parsed and SPIR64 LLVM IR emitted"
