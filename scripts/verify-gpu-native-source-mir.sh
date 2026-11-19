#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
head="$(git rev-parse HEAD)"
tree="$(git rev-parse 'HEAD^{tree}')"
if [[ -n "${EXPECTED_GPU_HEAD_SHA:-}" && "$head" != "$EXPECTED_GPU_HEAD_SHA" ]]; then
  echo "GPU_SOURCE_MIR_FAIL: wrong source head" >&2
  exit 1
fi
echo "evidence_class=HOST_ONLY_SOURCE_MIR"
echo "source_head=$head"
echo "source_tree=$tree"
echo "compiler_guest_executed=false"
echo "physical_gpu_executed=false"
if [[ -n "$(git status --porcelain --untracked-files=all)" ]]; then
  echo "GPU_SOURCE_MIR_FAIL: dirty source checkout" >&2
  exit 1
fi
for path in \
  src/main/cpp/oreslang/frontend/Parser.cpp \
  src/main/cpp/oreslang/frontend/GpuLaunchSourceMir.hpp \
  src/main/cpp/oreslang/frontend/GpuKernelSourceLink.hpp \
  src/main/cpp/oreslang/frontend/GpuKernelBodyHir.hpp \
  src/main/cpp/oreslang/runtime/GpuTypedLaunchIr.hpp \
  tests/gpu_source_launch_mir_test.cpp \
  tests/gpu_kernel_source_link_test.cpp \
  tests/gpu_kernel_body_hir_test.cpp \
  tests/atto_gpu_kernel_source_link_admission.cmake \
  tests/gpu_kernel_source_link_harness_selftest.sh \
  tests/gpu_source_mir_adversarial_test.cpp \
  tests/gpu_native_negative_harness_selftest.sh \
  tests/atto_gpu_explicit_launch_admission.cmake \
  scripts/verify-gpu-native-source-mir.sh \
  .github/workflows/gpu-native-source-mir.yml \
  CMakeLists.txt; do
  expected="$(git rev-parse "HEAD:$path")"
  observed="$(git hash-object "$path")"
  [[ "$expected" == "$observed" ]] || { echo "GPU_SOURCE_MIR_FAIL: blob mismatch $path" >&2; exit 1; }
  echo "blob:$path:$observed"
done
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
"${CXX:-c++}" --version | head -n1
"${CXX:-c++}" -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror -UNDEBUG \
  -Isrc/main/cpp tests/gpu_source_launch_mir_test.cpp -o "$work/gpu-source-mir"
"$work/gpu-source-mir"
"${CXX:-c++}" -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror -UNDEBUG \
  -Isrc/main/cpp tests/gpu_source_mir_adversarial_test.cpp -o "$work/gpu-source-mir-adversarial"
"$work/gpu-source-mir-adversarial"
"${CXX:-c++}" -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror -UNDEBUG \
  -Isrc/main/cpp tests/gpu_kernel_source_link_test.cpp -o "$work/gpu-kernel-source-link"
"$work/gpu-kernel-source-link"
"${CXX:-c++}" -std=c++20 -O2 -Wall -Wextra -Wpedantic -Werror -UNDEBUG \
  -Isrc/main/cpp tests/gpu_kernel_body_hir_test.cpp -o "$work/gpu-kernel-body-hir"
"$work/gpu-kernel-body-hir"
bash tests/gpu_kernel_source_link_harness_selftest.sh
bash tests/gpu_native_negative_harness_selftest.sh
echo "GPU_SOURCE_MIR_HOST_PASS: isolated host-only tests; native Parser.cpp and GPU device not executed"
