#!/usr/bin/env bash
# Source-only regression guard: a corrupted YAML workflow must not count as
# executable AttoLang/LLVM guest parity. This is NOT compiler-backed evidence.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
workflow="${1:-$root/.github/workflows/atto-native-harness-llvm.yml}"
die() { printf 'FAIL LLVM workflow contract: %s\n' "$*" >&2; exit 1; }
[[ -f "$workflow" && ! -L "$workflow" ]] || die 'workflow absent or symlinked'
count() { grep -Fc -- "$1" "$workflow" || :; }
unique() { [[ "$(count "$1")" -eq 1 ]] || die "expected exactly one: $1"; }
unique 'name: AttoLang LLVM native harness executable parity (O0 O2)'
unique '  atto-native-executable:'
unique 'Checkout exact source head without persistent credentials'
unique 'Bind exact commit and tree'
unique 'Verify source-free inventory and adversarial fixture mutations'
unique 'Build production LLVM AttoLang compiler and native security tests'
unique 'Verify CTest source and compiler-backed registrations are separate'
unique 'Execute native C++ import and stdlib registry security tests'
unique 'Execute 30 actual .atto guests at Clang O0 and O2'
unique 'Assert unchanged source and exact head after execution'
unique 'Preserve host-observed native evidence (not ORES_TEST guest receipts)'
unique 'bash tests/atto_native_harness_parity.sh --run'
unique "ATTO_LLVM_NATIVE_PARITY_SUBSET_EXECUTED|positive=30|optimizations=2|negative=8|guest_receipt_protocol=UNSUPPORTED"
unique 'atto_native_harness_compiler_guest_o0_o2'
unique 'atto_native_harness_inventory_source_only'
[[ "$(count 'native_project_root_import_security')" -eq 2 ]] || die 'expected one CTest inventory and one execution'
[[ "$(count 'native_core_library_registry_provider_policy')" -eq 2 ]] || die 'expected registry in CTest inventory and execution'
[[ "$(count 'test "$(git rev-parse HEAD)" = "$EXPECTED_HEAD"')" -eq 2 ]] || die 'expected two exact-head checks'
unique 'persist-credentials: false'
unique "ATTO_LLVM_GUEST_TREE=\$(git rev-parse 'HEAD^{tree}')"
unique 'ctest --test-dir build -N'
[[ "$(grep -Ec '^      - name:' "$workflow")" -eq 10 ]] ||
  die 'wrong step count or duplicate workflow sections'
[[ "$(grep -Ec '^    runs-on: ubuntu-24.04$' "$workflow")" -eq 1 ]] ||
  die 'missing or duplicated runner'
[[ "$(grep -Ec '^      - name: Execute 30 actual' "$workflow")" -eq 1 ]] ||
  die 'multiple native guest steps'
[[ "$(grep -Ec '^      - name: Preserve host-observed' "$workflow")" -eq 1 ]] ||
  die 'multiple evidence publishers'
if grep -Eq '^ "\$RUNNER_TEMP/|^ *shell: bash$' "$workflow"; then
  # shell: bash must be nested inside a step: a top-level shell key is invalid.
  if grep -Eq '^ "$RUNNER_TEMP/' "$workflow"; then
    die 'orphaned YAML/heredoc text'
  fi
fi
if [[ "$#" -eq 0 ]]; then
  tmp="$(mktemp -d)"
  trap 'rm -rf -- "$tmp"' EXIT
  cp "$workflow" "$tmp/fixture.yml"
  cat "$workflow" >> "$tmp/fixture.yml"
  if bash "$0" "$tmp/fixture.yml" >/dev/null 2>&1; then
    die 'duplicate entire workflow accepted'
  fi
  cp "$workflow" "$tmp/fixture.yml"
  sed -i '/^      - name: Execute 30 actual/d' "$tmp/fixture.yml"
  if bash "$0" "$tmp/fixture.yml" >/dev/null 2>&1; then
    die 'missing native guest runner step accepted'
  fi
  cp "$workflow" "$tmp/fixture.yml"
  sed -i 's/guest_receipt_protocol=UNSUPPORTED/guest_receipt_protocol=SUPPORTED/g' "$tmp/fixture.yml"
  if bash "$0" "$tmp/fixture.yml" >/dev/null 2>&1; then
    die 'false Java receipt parity accepted'
  fi
  cp "$workflow" "$tmp/fixture.yml"
  sed -i 's/persist-credentials: false/persist-credentials: true/g' "$tmp/fixture.yml"
  if bash "$0" "$tmp/fixture.yml" >/dev/null 2>&1; then
    die 'persistent GitHub checkout credentials accepted'
  fi
fi
echo 'PASS LLVM canonical .atto workflow source-only admission (four adversarial mutations)'
