#!/usr/bin/env bash
set -euo pipefail
if [[ "${ATTO_GPU_HW_REQUIRED:-}" != 1 ]]; then
  echo 'GPU_G3_FAIL: explicit ATTO_GPU_HW_REQUIRED=1 is mandatory' >&2; exit 1
fi
if [[ -z "${ATTO_G2_SOURCE_DIR:-}" ]]; then
  echo 'GPU_G3_FAIL: ATTO_G2_SOURCE_DIR to exact LLVM G2 compiler checkout is required' >&2; exit 1
fi
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
g2="$(cd "$ATTO_G2_SOURCE_DIR" && pwd)"
head="$(git -C "$root" rev-parse HEAD)"
tree="$(git -C "$root" rev-parse HEAD^{tree})"
g2_head="$(git -C "$g2" rev-parse HEAD)"
g2_tree="$(git -C "$g2" rev-parse HEAD^{tree})"
if [[ -n "${ATTO_EXPECTED_HEAD_SHA:-}" && "$head" != "$ATTO_EXPECTED_HEAD_SHA" ]]; then
  echo 'GPU_G3_FAIL: probe head mismatch' >&2; exit 1
fi
for path in 'tests/atto_gpu_physical_saxpy_probe.cpp' 'scripts/verify-atto-gpu-physical-saxpy.sh'; do
  [[ "$(git -C "$root" rev-parse "HEAD:$path")" == "$(git -C "$root" hash-object "$root/$path")" ]] || {
    echo "GPU_G3_FAIL: edited probe source $path" >&2; exit 1;
  }
done
[[ "$(git -C "$g2" hash-object "$g2/tools/atto_gpu_saxpy_kernelc.cpp")" == '364e06e8e20b6580e603cd89a1b81387ea2c89ae' ]] || {
  echo 'GPU_G3_FAIL: G2 compiler not pinned to reviewed blob' >&2; exit 1;
}
[[ "$(git -C "$g2" hash-object "$g2/tests/gpu-saxpy-g2/saxpy.atto")" == '48290eaac9f64358cfb0e1b792d8a63fa48ae150' ]] || {
  echo 'GPU_G3_FAIL: .atto fixture not pinned to canonical blob' >&2; exit 1;
}
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
clang++ -std=c++17 -Wall -Wextra -Wpedantic -Werror \
  "$g2/tools/atto_gpu_saxpy_kernelc.cpp" -o "$work/atto-gpu-saxpy-kernelc"
clang++ -std=c++17 -Wall -Wextra -Wpedantic -Werror \
  "$root/tests/atto_gpu_physical_saxpy_probe.cpp" -ldl -o "$work/gpu-physical-probe"
"$work/atto-gpu-saxpy-kernelc" "$g2/tests/gpu-saxpy-g2/saxpy.atto" "$work/device.cl"
digest="$(sha256sum "$work/device.cl" | cut -d' ' -f1)"
[[ "$digest" == 'b5076980355531632de445e05e35554d3fb54ceeebaf707457863571bdeb77a4' ]] || {
  echo 'GPU_G3_FAIL: generated device binary source does not match pinned artifact' >&2; exit 1;
}
clang -target spir64-unknown-unknown -x cl -cl-std=CL1.2 -S -emit-llvm \
  -o "$work/device.spir64.ll" "$work/device.cl"
printf 'probe_head %s\nprobe_tree %s\ng2_head %s\ng2_tree %s\nartifact_sha256 %s\n' \
  "$head" "$tree" "$g2_head" "$g2_tree" "$digest"
printf 'evidence_class OPENCL_REPORTED_GPU_DEVICE_EXECUTION_EXTERNAL_ATTESTATION_REQUIRED\n'
"$work/gpu-physical-probe" "$work/device.cl"
printf 'G3_OPENCL_DISPATCH_OBSERVED: external physical GPU attestation and production guest integration still required\n'
