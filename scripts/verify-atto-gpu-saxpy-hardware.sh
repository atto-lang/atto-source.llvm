#!/usr/bin/env bash
# Exact-tree physical GPU assurance gate. Must run in the ORIGINAL trusted
# source checkout with a compiled native LLVM/Atto binary and OpenCL GPU.
set -euo pipefail
if [ "$#" -ne 3 ]; then
  echo "usage: $0 <oreslang-llvmc> <opencl-saxpy-gpu-probe> <saxpy.atto>" >&2
  exit 64
fi
compiler="$1"
probe="$2"
fixture="$3"
for file in "$compiler" "$probe" "$fixture"; do
  if [ ! -f "$file" ]; then
    echo "missing exact-source GPU assurance input: $file" >&2
    exit 69
  fi
done
if ! command -v git >/dev/null || ! command -v sha256sum >/dev/null; then
  echo "git and sha256sum required for exact-source GPU assurance" >&2
  exit 69
fi
head_sha="$(git rev-parse --verify HEAD^{commit})"
head_tree="$(git rev-parse --verify HEAD^{tree})"
if [ -z "$head_sha" ] || [ -z "$head_tree" ]; then
  echo "missing exact source Git identity" >&2
  exit 69
fi
if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
  echo "tracked source tree dirty: refusing unsupported GPU source provenance" >&2
  exit 70
fi
if [ -n "${GITHUB_RUN_ID:-}" ]; then
  # github.sha is often a synthetic PR merge SHA. Callers MUST explicitly
  # bind the immutable PR source head, not just a branch label or merge ref.
  if [ -z "${ATTO_EXPECTED_PR_HEAD_SHA:-}" ] ||
     [ "${ATTO_EXPECTED_PR_HEAD_SHA}" != "$head_sha" ]; then
    echo "missing or mismatched immutable PR head identity" >&2
    exit 70
  fi
elif [ -n "${ATTO_EXPECTED_PR_HEAD_SHA:-}" ] &&
     [ "${ATTO_EXPECTED_PR_HEAD_SHA}" != "$head_sha" ]; then
  echo "local expected head identity mismatch" >&2
  exit 70
fi
workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT
"$compiler" --emit-opencl-gpu-v1 "$fixture" > "$workdir/atto-saxpy.cl"
if [ ! -s "$workdir/atto-saxpy.cl" ]; then
  echo "native Atto GPU compiler emitted no device artifact" >&2
  exit 1
fi
if ! grep -q '__kernel void atto_saxpy_f32' "$workdir/atto-saxpy.cl"; then
  echo "native compiler emitted unexpected GPU kernel" >&2
  exit 1
fi
set +e
"$probe" "$fixture" "$workdir/atto-saxpy.cl" > "$workdir/physical.log" 2>&1
probe_status="$?"
set -e
cat "$workdir/physical.log"
if [ "$probe_status" -eq 77 ]; then
  echo "NO_HARDWARE_EVIDENCE: incompatible or absent physical OpenCL GPU" >&2
  exit 69
fi
if [ "$probe_status" -ne 0 ]; then
  echo "PHYSICAL_GPU_FAILURE: native probe status $probe_status" >&2
  exit 1
fi
if ! grep -q '^ATTO_SAXPY_PHYSICAL_GPU_PASSED device=' "$workdir/physical.log"; then
  echo "no authenticated physical GPU completion marker" >&2
  exit 1
fi
# The human-readable receipt is not a signature. Hosted CI must additionally
# attach nonzero executed-job steps, run attempt and compiler toolchain digest.
printf 'ATTO_SAXPY_GPU_RECEIPT_V1\n'
printf 'head_sha=%s\n' "$head_sha"
printf 'head_tree=%s\n' "$head_tree"
printf 'source_sha256=%s\n' "$(sha256sum "$fixture" | cut -d' ' -f1)"
printf 'opencl_sha256=%s\n' "$(sha256sum "$workdir/atto-saxpy.cl" | cut -d' ' -f1)"
printf 'compiler_sha256=%s\n' "$(sha256sum "$compiler" | cut -d' ' -f1)"
printf 'probe_sha256=%s\n' "$(sha256sum "$probe" | cut -d' ' -f1)"
printf 'github_run_id=%s\n' "${GITHUB_RUN_ID:-not-hosted}"
printf 'github_run_attempt=%s\n' "${GITHUB_RUN_ATTEMPT:-not-hosted}"
printf 'hardware_event=source-compiled-physical-OpenCL-three-buffer\n'
