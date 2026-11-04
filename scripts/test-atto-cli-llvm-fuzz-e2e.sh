#!/usr/bin/env bash
# Cross-repo acceptance: REAL LLVM-built .atto target + installed Rust atto-test.
# This is intentionally not a synthetic compiler harness and does not mirror
# private repo contents into another GitHub organization. Exit nonzero when
# any compiler, auditor, CLI or observed guest property is unavailable.
set -euo pipefail
(( $# == 4 )) || {
  echo 'usage: test-atto-cli-llvm-fuzz-e2e.sh LLVM_COMPILER CLANG IR_AUDITOR ATTO_TEST_BIN' >&2
  exit 64
}
compiler="$1"
clang="$2"
auditor="$3"
atto_test="$4"
[[ -x "$compiler" && -x "$auditor" && -x "$atto_test" ]] ||
  { echo 'AttoLang compiler, IR auditor and Rust atto-test must be executable' >&2; exit 69; }
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT
project="$work/project"
mkdir -p "$project/build"
target="$project/build/fuzz-byte"
# mktemp defaults create a directory without YAML/TOML punctuation; refuse
# unusual output paths instead of constructing ambiguous TOML strings.
[[ "$target" != *'"'* && "$target" != *\\* && "$target" != *$'\n'* ]] ||
  { echo 'unsafe TOML target path' >&2; exit 64; }
cat > "$project/atto-test.toml" <<MANIFEST
[fuzz]
command = ["./build/fuzz-byte"]
max_bytes = 65536
inconclusive_exits = [2]
MANIFEST
export ORESLANG_LLVMC="$compiler"
export ORES_NO_GC_IR_AUDIT="$auditor"
export CLANG="$clang"
builder="$root/scripts/build-native-atto-fuzz-target.sh"
bash "$builder" "$root/tests/fixtures/atto-fuzz-byte.atto" "$target"
output="$work/cli.stdout"
errors="$work/cli.stderr"
"$atto_test" --root "$project" --fuzz --seed 1729 --cases 1000 >"$output" 2>"$errors" || {
  echo 'canonical compiled .atto fuzz guest did not pass the Rust CLI' >&2
  cat "$errors" >&2
  exit 1
}
grep -Fqx 'FUZZ PASS cases=1000 seed=1729 (observed inputs only)' "$output" ||
  { echo 'Rust fuzz CLI did not execute 1000 canonical native cases' >&2; exit 1; }
[[ ! -s "$errors" && ! -d "$project/.oreslang-test/failures" ]] ||
  { echo 'canonical native fuzz guest unexpectedly produced findings' >&2; exit 1; }

# Replace with a compiler-generated intentionally wrong guest. Rust must
# reproduce, minimize and persist a replayable native byte finding.
bash "$builder" "$root/tests/fixtures/atto-fuzz-byte-mutant.atto" "$target"
set +e
"$atto_test" --root "$project" --fuzz --seed 1729 --cases 8 >"$output" 2>"$errors"
verdict=$?
set -e
[[ "$verdict" == 1 ]] ||
  { echo "mutant should have a real fuzz defect (observed=$verdict)" >&2; exit 1; }
grep -Fq 'FUZZ FAILURE seed=1729 ' "$output" ||
  { echo 'Rust CLI failed to report reproducible native guest error' >&2; exit 1; }
grep -Fq 'finding=Exit(77)' "$output" ||
  { echo 'native guest failure was reclassified or lost' >&2; exit 1; }
reproducer="$(sed -n 's/^.* path=\(.*\)$/\1/p' "$output" | tail -n 1)"
[[ -n "$reproducer" && "$reproducer" == "$project/.oreslang-test/failures/"* ]] ||
  { echo 'fuzz reproducer path escaped the project-local artifact directory' >&2; exit 1; }
[[ -f "$reproducer" && ! -L "$reproducer" ]] ||
  { echo 'missing or symlinked native fuzz reproducer' >&2; exit 1; }
size="$(wc -c < "$reproducer")"
[[ "$size" == 1 ]] ||
  { echo "LLVM fuzzer failed to shrink deterministic single-byte mismatch ($size bytes)" >&2; exit 1; }
set +e
"$target" < "$reproducer" > "$work/replay.stdout" 2>"$work/replay.stderr"
replayed=$?
set -e
[[ "$replayed" == 77 && ! -s "$work/replay.stdout" ]] ||
  { echo 'saved native byte finding did not replay on the LLVM executable' >&2; exit 1; }
grep -Fq 'ATTO_LLVM_FUZZ_MISMATCH position=0' "$work/replay.stderr" ||
  { echo 'replayed LLVM native guest did not report the expected mismatch' >&2; exit 1; }
printf 'ATTO_LLVM_CLI_E2E_PASS cases=1000 seed=1729 reproducer_bytes=1 actual_guest=1\n'
