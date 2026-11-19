#!/usr/bin/env bash
# Compiler-backed AttoLang *scalar predicate* smoke, not runtime protocol parity.
# Build oreslang-llvmc + no-GC IR audit from this exact source head first.
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
: "${ATTO_POLICY_EXPECTED_HEAD:?Set exact source Git HEAD}"
: "${ATTO_POLICY_RECEIPT:?Set output receipt path}"
[[ "$ATTO_POLICY_EXPECTED_HEAD" =~ ^[0-9a-f]{40}$ ]] || exit 64
[[ "$(git -C "$root" rev-parse HEAD)" == "$ATTO_POLICY_EXPECTED_HEAD" ]] || {
  echo "Compiler source head changed" >&2; exit 78;
}
script_path="scripts/test-formal-policy-atto-native.sh"
script_blob="$(git -C "$root" rev-parse "HEAD:$script_path")"
[[ "$(git -C "$root" hash-object "$root/$script_path")" == "$script_blob" ]] || {
  echo "Execution harness does not match exact source head" >&2
  exit 78
}
command -v jq >/dev/null || { echo "jq required" >&2; exit 69; }
command -v sha256sum >/dev/null || { echo "sha256sum required" >&2; exit 69; }
compiler="${ORESLANG_LLVMC:-$root/build/oreslang-llvmc}"
auditor="${ORES_NO_GC_IR_AUDIT:-$root/build/oreslang-no-gc-ir-audit}"
[[ -x "$compiler" && -x "$auditor" ]] || {
  echo "Real compiler and native IR auditor must be built" >&2; exit 69;
}
command -v "${CLANG:-clang}" >/dev/null || { echo "Clang required" >&2; exit 69; }

work="$(mktemp -d "${TMPDIR:-/tmp}/atto-policy-guest.XXXXXXXX")"
trap 'rm -rf -- "$work"' EXIT
case_file="$work/native-results.tsv"
: > "$case_file"
cases=(lease-fencing nats-token auth-destination epoch-atomicity dnd-drop-admission)
for name in "${cases[@]}"; do
  file="tests/formal-policy-atto/$name.atto"
  pinned_blob="$(git -C "$root" rev-parse "HEAD:$file")"
  actual_blob="$(git -C "$root" hash-object "$root/$file")"
  [[ "$actual_blob" == "$pinned_blob" ]] || {
    echo "Guest source blob does not match exact head: $file" >&2; exit 78;
  }
  for opt in 0 2; do
    native="$work/$name-O$opt"
    echo "COMPILE_GUEST $name opt=$opt blob=$pinned_blob"
    ORESLANG_LLVM_AOT_OPT_LEVEL="$opt" \
      "$root/scripts/compile-aot.sh" "$root/$file" "$native"
    [[ -x "$native" ]] || { echo "No native executable: $name" >&2; exit 1; }
    # The guest process itself must exit 0; host simulation is forbidden.
    if "$native"; then status=0; else status=$?; fi
    [[ "$status" == 0 ]] || {
      echo "NATIVE_GUEST_FAILURE $name opt=$opt exit=$status" >&2; exit 1;
    }
    binary_sha="$(sha256sum "$native" | cut -d' ' -f1)"
    printf '%s\t%s\t%s\t%s\t%s\n' "$name" "$opt" "$pinned_blob" "$binary_sha" "$status" >> "$case_file"
  done
done
# Reject independent effect/type/resolution mutations through the real compiler.
# Require a normal nonzero compile error, not a crash, missing compiler or
# emitted LLVM IR. Each fixture is checked against its exact Git blob.
negative_file="$work/negative-results.tsv"
: > "$negative_file"
negative_cases=(
  undeclared-authority
  legacy-async-modifier
  critical-suspends-modifier
  suspends-critical-modifier
  unbound-value
  implicit-string-to-int
)
for name in "${negative_cases[@]}"; do
  bad="tests/formal-policy-atto/negative/$name.atto"
  source_blob="$(git -C "$root" rev-parse "HEAD:$bad")"
  [[ "$(git -C "$root" hash-object "$root/$bad")" == "$source_blob" ]] || {
    echo "Negative source blob does not match exact head: $bad" >&2
    exit 78
  }
  stderr="$work/$name.stderr"
  output="$work/$name.ll"
  if "$compiler" --target=host "$root/$bad" > "$output" 2> "$stderr"; then
    echo "Compiler improperly accepted forbidden source: $name" >&2
    exit 1
  else
    status=$?
  fi
  [[ "$status" -gt 0 && "$status" -lt 128 ]] || {
    echo "Compiler crashed or was unavailable for negative case: $name (exit=$status)" >&2
    exit 1
  }
  [[ ! -s "$output" && -s "$stderr" ]] || {
    echo "Negative source emitted IR or lacked a diagnostic: $name" >&2
    exit 1
  }
  grep -Eiq '(error|failed|invalid|unsupported|unknown|undeclared|unrecognized)' "$stderr" || {
    echo "Negative source had no recognizable error diagnostic: $name" >&2
    exit 1
  }
  diagnostic_sha="$(sha256sum "$stderr" | cut -d' ' -f1)"
  printf '%s\t%s\t%s\t%s\n' "$name" "$source_blob" "$status" "$diagnostic_sha" >> "$negative_file"
done

mkdir -p "$(dirname "$ATTO_POLICY_RECEIPT")"
source_tree="$(git -C "$root" rev-parse 'HEAD^{tree}')"
compiler_sha="$(sha256sum "$compiler" | cut -d' ' -f1)"
auditor_sha="$(sha256sum "$auditor" | cut -d' ' -f1)"
jq -Rn \
  --arg sourceHead "$ATTO_POLICY_EXPECTED_HEAD" \
  --arg sourceTree "$source_tree" \
  --arg scriptGitBlob "$script_blob" \
  --arg compilerSha256 "$compiler_sha" \
  --arg auditorSha256 "$auditor_sha" \
  --arg runId "${GITHUB_RUN_ID:-local}" \
  --arg runAttempt "${GITHUB_RUN_ATTEMPT:-0}" \
  --rawfile negativeRows "$negative_file" \
  '[inputs | select(length > 0) | split("\t") | {
      fixture: .[0], optimization: (.[1]|tonumber),
      sourceGitBlob: .[2], executableSha256: .[3],
      guestExitStatus: (.[4]|tonumber)
    }] | {
      schema: "atto-native-policy-guest-receipt/v1",
      backend: "llvm-native",
      sourceHead: $sourceHead, sourceTree: $sourceTree,
      scriptGitBlob: $scriptGitBlob,
      compilerSha256: $compilerSha256, auditorSha256: $auditorSha256,
      runId: $runId, runAttempt: ($runAttempt|tonumber),
      nativeGuestExecutions: ., nativeGuestCount: length,
      rejectedUndeclaredSymbol: true,
      negativeCompilerRejections: ($negativeRows | split("\n") | map(select(length > 0)) |
        map(split("\t") | {
          fixture: .[0], sourceGitBlob: .[1], compilerExitStatus: (.[2] | tonumber),
          diagnosticSha256: .[3]
        })),
      negativeCount: ($negativeRows | split("\n") | map(select(length > 0)) | length),
      guestScope: "pure scalar predicates; not actor, network, or persistence parity"
    }' "$case_file" > "$ATTO_POLICY_RECEIPT"
jq -e '.schema == "atto-native-policy-guest-receipt/v1"
  and .nativeGuestCount == 10
  and (.nativeGuestExecutions | all(.guestExitStatus == 0))
  and .rejectedUndeclaredSymbol == true
  and .negativeCount == 6
  and (.negativeCompilerRejections | all(.compilerExitStatus > 0 and .compilerExitStatus < 128))
  and (.negativeCompilerRejections | all(.diagnosticSha256 | test("^[0-9a-f]{64}$")))
  and (.scriptGitBlob | test("^[0-9a-f]{40}$"))
  and .sourceHead != ""
  and .compilerSha256 != ""' "$ATTO_POLICY_RECEIPT" >/dev/null
echo "ATTO_NATIVE_GUEST_EXECUTION_PASS cases=8 negative_compile=6"
