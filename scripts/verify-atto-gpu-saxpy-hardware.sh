#!/usr/bin/env bash
# Strict, original-source physical GPU release gate (Linux sha256sum runner).
# Always build fresh binaries from the exact immutable Git tree. Never accept
# caller-supplied compiler/probe binaries or a caller-supplied source fixture.
set -euo pipefail
if [ "$#" -ne 0 ]; then
  echo "usage: $0 (no arguments; builds exact-source tools in a fresh directory)" >&2
  exit 64
fi
for command in git cmake sha256sum mktemp tar; do
  if ! command -v "$command" >/dev/null 2>&1; then
    echo "GPU assurance missing required trusted tool: $command" >&2
    exit 69
  fi
done
repo_root="$(git rev-parse --show-toplevel)"
cd "$repo_root"
head_sha="$(git rev-parse --verify HEAD^{commit})"
head_tree="$(git rev-parse --verify HEAD^{tree})"
if [ -z "$head_sha" ] || [ -z "$head_tree" ]; then
  echo "source Git commit/tree identity unavailable" >&2
  exit 69
fi
# Reject modified tracked sources, then build from an exact Git archive.
# Ignored/untracked local headers cannot enter this isolated compilation.
if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
  echo "tracked source tree dirty; refusing GPU hardware attestation" >&2
  exit 70
fi
tracked_fixture="tests/fixtures/atto-gpu-saxpy-f32.atto"
fixture="$tracked_fixture"
if [ ! -f "$fixture" ] || [ -L "$fixture" ]; then
  echo "canonical, regular .atto fixture missing" >&2
  exit 70
fi
expected_blob="$(git rev-parse "HEAD:$fixture")"
actual_blob="$(git hash-object -- "$fixture")"
if [ -z "$expected_blob" ] || [ "$expected_blob" != "$actual_blob" ]; then
  echo "canonical fixture differs from immutable Git object" >&2
  exit 70
fi
if [ -n "${GITHUB_RUN_ID:-}" ]; then
  if [ -z "${ATTO_EXPECTED_PR_HEAD_SHA:-}" ] ||
     [ "${ATTO_EXPECTED_PR_HEAD_SHA}" != "$head_sha" ]; then
    echo "hosted GPU runner must check out the exact immutable PR head" >&2
    exit 70
  fi
  case "${GITHUB_RUN_ATTEMPT:-}" in
    ""|0|*[!0-9]*) echo "hosted GPU run attempt unavailable" >&2; exit 70 ;;
  esac
  if [ "${GITHUB_REPOSITORY:-}" != "ores-truffle-oreslang/oreslang-source.llvm" ]; then
    echo "hosted GPU source repo identity mismatched" >&2
    exit 70
  fi
elif [ -n "${ATTO_EXPECTED_PR_HEAD_SHA:-}" ] &&
     [ "${ATTO_EXPECTED_PR_HEAD_SHA}" != "$head_sha" ]; then
  echo "local expected exact head identity mismatched" >&2
  exit 70
fi

workdir="$(mktemp -d)"
trap 'rm -rf "$workdir"' EXIT
source_dir="$workdir/source"
mkdir -p "$source_dir"
git archive --format=tar HEAD | tar -xf - -C "$source_dir"
fixture="$source_dir/$tracked_fixture"
if [ ! -f "$fixture" ] || [ -L "$fixture" ] ||
   [ "$(git hash-object -- "$fixture")" != "$expected_blob" ]; then
  echo "archived immutable GPU source is missing or corrupted" >&2
  exit 70
fi
build="$workdir/build"
# Fresh builds keep arbitrary stale/user-provided binaries out of the trusted
# hardware gate. A green fake script cannot be substituted as the compiler
# or physical probe. A separate trusted runner must still attest toolchain.
cmake -S "$source_dir" -B "$build" -DBUILD_TESTING=ON
cmake --build "$build" --parallel 2 --target \
  oreslang-llvmc oreslang-opencl-saxpy-gpu-probe \
  oreslang-opencl-saxpy-provider-v1-test \
  oreslang-atto-gpu-saxpy-source-v1-test
ctest --test-dir "$build" --output-on-failure -R   '^(atto_gpu_saxpy_source_v1|atto_gpu_saxpy_opencl_compiler_v1|atto_gpu_saxpy_provider_host_v1)$'
compiler="$build/oreslang-llvmc"
probe="$build/oreslang-opencl-saxpy-gpu-probe"
if [ ! -x "$compiler" ] || [ ! -x "$probe" ]; then
  echo "fresh source build produced no runnable GPU compiler/probe" >&2
  exit 1
fi
"$compiler" --emit-opencl-gpu-v1 "$fixture" > "$workdir/atto-saxpy.cl"
if [ ! -s "$workdir/atto-saxpy.cl" ] ||
   ! grep -q '__kernel void atto_saxpy_f32' "$workdir/atto-saxpy.cl"; then
  echo "Atto source compiler did not emit the expected device kernel" >&2
  exit 1
fi
set +e
"$probe" "$fixture" "$workdir/atto-saxpy.cl" > "$workdir/physical.log" 2>&1
probe_status="$?"
set -e
cat "$workdir/physical.log"
if [ "$probe_status" -eq 77 ]; then
  echo "NO_HARDWARE_EVIDENCE: no compatible physical OpenCL GPU" >&2
  exit 69
fi
if [ "$probe_status" -ne 0 ] ||
   ! grep -q '^ATTO_SAXPY_PHYSICAL_GPU_PASSED device=' "$workdir/physical.log"; then
  echo "PHYSICAL_GPU_FAILURE: actual kernel completion/oracle not proven" >&2
  exit 1
fi
# These hashes are source/build provenance data, not cryptographic signatures
# of hardware execution. The hosted job must attach native logs, runner
# identity, run attempt, nonzero executed steps and independent approval.
printf 'ATTO_SAXPY_GPU_RECEIPT_V1\n'
printf 'head_sha=%s\n' "$head_sha"
printf 'head_tree=%s\n' "$head_tree"
printf 'source_git_blob=%s\n' "$expected_blob"
printf 'source_sha256=%s\n' "$(sha256sum "$fixture" | cut -d' ' -f1)"
printf 'opencl_sha256=%s\n' "$(sha256sum "$workdir/atto-saxpy.cl" | cut -d' ' -f1)"
printf 'compiler_sha256=%s\n' "$(sha256sum "$compiler" | cut -d' ' -f1)"
printf 'probe_sha256=%s\n' "$(sha256sum "$probe" | cut -d' ' -f1)"
printf 'github_run_id=%s\n' "${GITHUB_RUN_ID:-not-hosted}"
printf 'github_run_attempt=%s\n' "${GITHUB_RUN_ATTEMPT:-not-hosted}"
printf 'hardware_event=source-compiled-physical-OpenCL-three-buffer\n'
