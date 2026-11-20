#!/usr/bin/env bash
# Hardware-required prerequisite for first GPU actor execution.
# This runs a native C++ OpenCL probe and kernel, NOT a compiled AttoLang actor.
# A missing/non-GPU OpenCL device fails: never re-label a CPU runtime as GPU.
set -euo pipefail
if [[ "$#" -ne 2 || ! -x "$1" || ! -f "$2" ]]; then
  echo "GPU_ACTOR_G1_USAGE: provide compiled physical probe and kernel file" >&2
  exit 2
fi
probe="$1"
kernel="$2"
log="$(mktemp)"
trap 'rm -f "$log"' EXIT
if ! "$probe" "$kernel" >"$log" 2>&1; then
  cat "$log" >&2
  echo "GPU_ACTOR_G1_PHYSICAL_GPU_UNVERIFIED: no verified device execution; no CPU fallback" >&2
  exit 3
fi
cat "$log"
grep -q '^GPU_OPENCL_REPORTED_GPU_SAXPY_PASS ' "$log"
grep -q 'kernel_event_status=CL_COMPLETE' "$log"
grep -q 'CPU_FALLBACK=false' "$log"
grep -q 'HARDWARE_ATTESTATION=EXTERNAL_REQUIRED' "$log"
echo "GPU_ACTOR_G1_KERNEL_PREREQUISITE_PASS: native kernel only, actor guest unverified"
