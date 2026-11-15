#!/usr/bin/env bash
# Candidate exact-source .atto Task guest test runner. No cross-org checkout or mirror.
# Existing local compiler and explicitly supplied shared conformance checkout only.
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
: "${ATTO_TASK_SUITE_ROOT:?Provide an authorized, source-local shared conformance checkout}"
: "${ATTO_TASK_SUITE_SHA:?Pin the exact conformance Git HEAD (40 hex chars)}"
[[ "$ATTO_TASK_SUITE_SHA" =~ ^[0-9a-f]{40}$ ]] || { echo 'Invalid suite SHA' >&2; exit 64; }
suite="$(cd -- "$ATTO_TASK_SUITE_ROOT" && pwd -P)"
[[ -f "$suite/bin/portable-task-v1-run.mjs" ]] || { echo 'Shared .atto runner missing' >&2; exit 69; }
[[ "$(git -C "$suite" rev-parse HEAD)" == "$ATTO_TASK_SUITE_SHA" ]] || { echo 'Suite HEAD mismatch' >&2; exit 78; }
[[ "$(git -C "$root" rev-parse HEAD)" =~ ^[0-9a-f]{40}$ ]] || exit 78
command -v node >/dev/null || { echo 'Node >=22 required' >&2; exit 69; }
# LLVM must be built from the exact checked-out source, with audit and Clang.
[[ -x "$root/build/oreslang-llvmc" && -x "$root/build/oreslang-no-gc-ir-audit" ]] || { echo 'Build LLVM compiler and no-GC IR auditor first' >&2; exit 69; }

config="$(mktemp "${TMPDIR:-/tmp}/atto-task-llvm.XXXXXXXX.json")"
trap 'rm -f -- "$config"' EXIT
export ATTO_RUNNER_COMPILER_SHA="$(git -C "$root" rev-parse HEAD)"
export ATTO_RUNNER_ROOT="$root" ATTO_RUNNER_SUITE_SHA="$ATTO_TASK_SUITE_SHA"
export ATTO_RUNNER_CONFIG="$config" ATTO_RUNNER_BACKEND="llvm"

node --input-type=module -e '
  import {writeFileSync} from "node:fs";
  const e=process.env,root=e.ATTO_RUNNER_ROOT;
  const isJava=e.ATTO_RUNNER_BACKEND==="java";
  const probe=root+"/contracts/v1/java_probe.py";
  const identity=isJava?root+"/target/classes/dev/oreslang/launcher/OresMain.class":root+"/build/oreslang-llvmc";
  const cfg={
    backend:e.ATTO_RUNNER_BACKEND,compilerRoot:root,
    compilerCommit:e.ATTO_RUNNER_COMPILER_SHA,fixtureCommit:e.ATTO_RUNNER_SUITE_SHA,
    compilerIdentity:identity,artifactPath:isJava?identity:"{out}",
    compileArgv:isJava?[e.ATTO_RUNNER_PYTHON,probe,"{file}"]:[root+"/scripts/compile-aot.sh","{file}","{out}"],
    guestArgv:isJava?[e.ATTO_RUNNER_PYTHON,probe,"{file}","--execute"]:["{out}"]
  };
  writeFileSync(e.ATTO_RUNNER_CONFIG,JSON.stringify(cfg)+"\n",{mode:0o600});
'
node "$suite/bin/portable-task-v1-run.mjs" "--config=$config"
