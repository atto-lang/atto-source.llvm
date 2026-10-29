#!/usr/bin/env bash
# Run only on an approved source-local host with a clean exact-head checkout.
# This is the wasm32 compiled actor lifecycle smoke, NOT six-case isolation parity.
set -euo pipefail

fail() { printf 'NOT_EXECUTED: %s\n' "$*" >&2; exit 1; }

[[ $# -eq 0 ]] || fail 'no positional flags permitted; set EXPECTED_HEAD explicitly'
[[ "${EXPECTED_HEAD:-}" =~ ^[0-9a-f]{40}$ ]] ||
  fail 'EXPECTED_HEAD must be an independently reviewed full commit SHA'

for tool in git cmake ctest python3 node clang; do
  command -v "$tool" >/dev/null 2>&1 || fail "required tool unavailable: $tool"
done

root="$(git rev-parse --show-toplevel)" || fail 'no source checkout'
[[ -d "$root" && ! -L "$root" && "$(pwd -P)" == "$root" ]] ||
  fail 'execute from the real source checkout root'
[[ "$(git rev-parse HEAD)" == "$EXPECTED_HEAD" ]] ||
  fail 'checkout differs from reviewed EXPECTED_HEAD'
[[ "$(git rev-parse 'HEAD^{tree}')" == "$(git show -s --format=%T HEAD)" ]] ||
  fail 'Git source tree identity mismatch'
tracked="$(git status --porcelain --untracked-files=no)" ||
  fail 'tracked Git source inventory unavailable'
[[ -z "$tracked" ]] || fail 'tracked source checkout modified'
[[ ! -e "$root/build" && ! -L "$root/build" ]] ||
  fail 'requires a fresh checkout with no preexisting build directory'
untracked="$(git ls-files --others --exclude-standard)" ||
  fail 'untracked Git source inventory unavailable'
[[ -z "$untracked" ]] || fail 'untracked checkout paths exist before build'
# Git-ignored files outside build can override compiler headers/modules without
# affecting the exact Git root tree. A fresh source build admits none.
ignored="$(git ls-files --others --ignored --exclude-standard)" ||
  fail 'ignored Git source inventory unavailable'
[[ -z "$ignored" ]] || fail 'ignored checkout paths exist before build'
receipt_dir="$(mktemp -d "${TMPDIR:-/tmp}/ores-llvm-actor-receipt.XXXXXXXX")"
trap 'rm -rf -- "$receipt_dir"' EXIT
receipt="$receipt_dir/compiler.json"

cmake -S "$root" -B "$root/build" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build "$root/build" --parallel 2

python3 "$root/tests/verify_compiler_receipt.py" capture \
  --source "$root" --compiler "$root/build/oreslang-llvmc" \
  --expected-head "$EXPECTED_HEAD" --receipt "$receipt"

python3 -m unittest discover -s "$root/tests" -p test_verify_compiler_receipt.py -v
python3 -m unittest discover -s "$root/tests" -p test_require_compiled_actor_ctest.py -v
python3 -m unittest discover -s "$root/tests" -p test_verify_ctest_execution.py -v
python3 -m unittest discover -s "$root/tests" -p test_llvm_ci_actor_workflow.py -v

python3 "$root/tests/require_compiled_actor_ctest.py" \
  --source-dir "$root" --build-dir "$root/build"
ctest --test-dir "$root/build" -R '^wasm32_source_actor_guest_lifecycle$' \
  --output-on-failure --timeout 120
python3 "$root/tests/verify_compiler_receipt.py" verify \
  --source "$root" --compiler "$root/build/oreslang-llvmc" \
  --expected-head "$EXPECTED_HEAD" --receipt "$receipt"

python3 "$root/tests/ctest_manifest_gate.py" --build-dir "$root/build" --min-tests 250
ctest --test-dir "$root/build" --output-on-failure --timeout 240 --parallel 2 \
  --output-junit "$receipt_dir/full-ctest.xml"
python3 "$root/tests/verify_ctest_execution.py" \
  --build-dir "$root/build" --junit "$receipt_dir/full-ctest.xml" --min-tests 250
python3 "$root/tests/verify_compiler_receipt.py" verify \
  --source "$root" --compiler "$root/build/oreslang-llvmc" \
  --expected-head "$EXPECTED_HEAD" --receipt "$receipt"

printf 'SOURCE_LOCAL_COMPILED_ACTOR_LIFECYCLE_AND_FULL_CTEST_PASS HEAD=%s TREE=%s\n' \
  "$EXPECTED_HEAD" "$(git rev-parse 'HEAD^{tree}')"
