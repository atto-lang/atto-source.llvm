#!/usr/bin/env bash
# Source-only regression guard: a corrupted YAML workflow must not count as
# executable AttoLang/LLVM guest parity. This is NOT compiler-backed evidence.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
workflow="${1:-$root/.github/workflows/atto-native-harness-llvm.yml}"
die() { printf 'FAIL LLVM workflow contract: %s\n' "$*" >&2; exit 1; }
[[ -f "$workflow" && ! -L "$workflow" ]] || die 'workflow absent or symlinked'
count() { grep -Fc -- "$1" "$workflow" || :; }
runner="${ATTO_NATIVE_HARNESS_SCRIPT:-$root/tests/atto_native_harness_parity.sh}"
[[ -f "$runner" && ! -L "$runner" ]] || die 'native source harness missing'
gate_line() { grep -nF -- "$1" "$runner" | head -n 1 | cut -d: -f1; }
audit_line="$(gate_line 'timeout 20s "$auditor" "$ir"')"
clang_line="$(gate_line 'timeout 60s "$clang_exe" "-O$level"')"
compare_line="$(gate_line 'cmp -s "$tmp/$id-O0.ll" "$tmp/$id-O2.ll"')"
receipt_line="$(gate_line "printf 'ATTO_LLVM_NATIVE_HOST_OBSERVED|")"
[[ -n "$audit_line" && -n "$clang_line" && -n "$compare_line" && -n "$receipt_line" ]] ||
  die 'missing production no-GC IR audit, Clang, IR comparison or receipt'
(( audit_line < clang_line && clang_line < compare_line && compare_line < receipt_line )) ||
  die 'unsafe native guest audit or receipt ordering'

unique() { [[ "$(count "$1")" -eq 1 ]] || die "expected exactly one: $1"; }
# Reject historical duplicate/truncated workflow checker source.
[[ "$(grep -c "^echo 'PASS LLVM canonical" "$0")" -eq 1 ]] ||
  die 'workflow checker source contains duplicated completion path'
[[ "$(grep -c '^unique "tests/atto_native_guest_receipts.sh"' "$0")" -eq 1 ]] ||
  die 'workflow checker source contains duplicated guest assertions'
unique 'name: AttoLang LLVM native harness executable parity (O0 O2)'
# The checked-in AttoLang source must byte-match the compiler-owned
# embedded canonical library; host or symlink fallback cannot substitute it.
python3 - "$root/stdlib/testing_m1.atto" \
  "$root/src/main/cpp/oreslang/frontend/CoreLibrarySources.hpp" <<'PY'
from pathlib import Path
import sys
source, header = (Path(p) for p in sys.argv[1:])
if not source.is_file() or source.is_symlink() or not header.is_file() or header.is_symlink():
    raise SystemExit("M1 compiler source is absent or symlinked")
text = header.read_text()
start, end = 'R"ATTO_M1(', ')ATTO_M1";'
if text.count(start) != 1 or text.count(end) != 1:
    raise SystemExit("missing or duplicated compiler-owned M1 source")
embedded = text.split(start, 1)[1].split(end, 1)[0]
if embedded != source.read_text():
    raise SystemExit("compiler-owned M1 source drift")
PY

unique '  atto-native-executable:'
unique "      - 'CMakeLists.txt'"
unique "      - 'cmake/**'"
unique "      - 'src/main/**'"
unique "      - 'tests/**'"
unique "      - 'scripts/**'"
unique "      - 'stdlib/**'"
unique 'Checkout exact source head without persistent credentials'
unique 'Bind exact commit and tree'
unique 'Verify source-free inventory and adversarial fixture mutations'
unique 'Build production LLVM AttoLang compiler and native security tests'
unique 'Verify CTest source and compiler-backed registrations are separate'
unique 'Execute native C++ import security tests'
unique 'Execute 40 actual .atto guests at Clang O0 and O2'
unique 'Assert unchanged source and exact head after execution'
unique 'Preserve host-observed native evidence (not ORES_TEST guest receipts)'
unique 'bash tests/atto_native_harness_parity.sh --run'
unique 'ATTO_LLVM_GUEST_RECEIPT_SMOKE_EXECUTED|suites=14|optimizations=2|native=28|testkit=PARTIAL_M1'
[[ "$(grep -Ec "^          test .*ATTO_LLVM_NATIVE_HOST_OBSERVED.*-eq 80$" "$workflow")" -eq 1 ]] ||
  die 'native scalar guest count is not 80'
unique "grep -c '^ATTO_LLVM_GUEST_RECEIPT_HOST_OBSERVED|'"
[[ "$(grep -Ec "^          test .*ATTO_LLVM_GUEST_RECEIPT_HOST_OBSERVED.*-eq 28$" "$workflow")" -eq 1 ]] ||
  die 'expected exactly 28 compiled native guest receipt executions'
unique "tests/atto_native_guest_receipts.sh"
unique "tests/verify_atto_guest_receipts.py"
[[ "$(grep -Fc 'python3 "$root/tests/test_atto_native_guest_receipts_cli.py"' "$root/tests/test_atto_native_parity_manifest.sh")" -eq 1 ]] ||
  die 'native fixture inventory omits subprocess CLI positive/negative controls'
[[ "$(grep -Fc 'PYTHONOPTIMIZE=2 python3 "$root/tests/test_atto_native_guest_receipts_cli.py"' "$root/tests/test_atto_native_parity_manifest.sh")" -eq 1 ]] ||
  die 'native fixture inventory omits optimized-mode CLI controls'
unique 'ORES_NO_GC_IR_AUDIT="$PWD/build/oreslang-no-gc-ir-audit"'
unique 'oreslang-no-gc-ir-audit-build'
unique 'oreslang-core-library-sources-test'
unique 'native_core_library_registry_provider_policy'
unique 'src/main/cpp/oreslang/frontend/CoreLibrarySources.hpp'
unique 'tests/core_library_sources_test.cpp'
unique 'oreslang-no-gc-ir-audit-tests-build'
unique "ctest --test-dir build --output-on-failure --no-tests=error -R '^(native_no_gc_ir_audit_unit|native_core_library_registry_provider_policy)$'"
unique "ATTO_LLVM_NATIVE_PARITY_SUBSET_EXECUTED|positive=40|optimizations=2|negative=10|guest_receipt_protocol=UNSUPPORTED"
unique 'atto_native_harness_compiler_guest_o0_o2'
unique 'atto_native_harness_inventory_source_only'
[[ "$(count 'native_project_root_import_security')" -eq 2 ]] || die 'expected one CTest inventory and one execution'
[[ "$(count 'test "$(git rev-parse HEAD)" = "$EXPECTED_HEAD"')" -eq 2 ]] || die 'expected two exact-head checks'
unique 'persist-credentials: false'
unique "ATTO_LLVM_GUEST_TREE=\$(git rev-parse 'HEAD^{tree}')"
unique 'ctest --test-dir build -N'
[[ "$(grep -Ec '^      - name:' "$workflow")" -eq 10 ]] ||
  die 'wrong step count or duplicate workflow sections'
[[ "$(grep -Ec '^    runs-on: ubuntu-24.04$' "$workflow")" -eq 1 ]] ||
  die 'missing or duplicated runner'
[[ "$(grep -Ec '^      - name: Execute 40 actual' "$workflow")" -eq 1 ]] ||
  die 'multiple native guest steps'
[[ "$(grep -Ec '^      - name: Preserve host-observed' "$workflow")" -eq 1 ]] ||
  die 'multiple evidence publishers'
if grep -Eq '^ "\$RUNNER_TEMP/|^ *shell: bash$' "$workflow"; then
  # shell: bash must be nested inside a step: a top-level shell key is invalid.
  if grep -Eq '^ "$RUNNER_TEMP/' "$workflow"; then
    die 'orphaned YAML/heredoc text'
  fi
fi
# Parse all YAML run blocks with bash -n; marker-count checks cannot detect
# unterminated shell quoting in the real CI workflow.
python3 - "$workflow" <<'PY'
import pathlib
import subprocess
import sys
lines = pathlib.Path(sys.argv[1]).read_text().splitlines()
blocks = []
for i, line in enumerate(lines):
    if line != "        run: |":
        continue
    body = []
    for next_line in lines[i + 1:]:
        if next_line.startswith("          "):
            body.append(next_line[10:])
        elif not next_line:
            body.append("")
        else:
            break
    blocks.append("\n".join(body) + "\n")
if len(blocks) != 8:
    raise SystemExit("wrong CI shell block count: " + str(len(blocks)))
for i, block in enumerate(blocks, 1):
    result = subprocess.run(["bash", "-n"], input=block, text=True,
                            capture_output=True, check=False)
    if result.returncode:
        raise SystemExit("bad CI shell block %d: %s" % (i, result.stderr))
PY

if [[ "$#" -eq 0 ]]; then
  tmp="$(mktemp -d)"
  trap 'rm -rf -- "$tmp"' EXIT
  cp "$workflow" "$tmp/fixture.yml"
  cat "$workflow" >> "$tmp/fixture.yml"
  if bash "$0" "$tmp/fixture.yml" >/dev/null 2>&1; then
    die 'duplicate entire workflow accepted'
  fi
  cp "$workflow" "$tmp/fixture.yml"
  sed -i '/^      - name: Execute 40 actual/d' "$tmp/fixture.yml"
  if bash "$0" "$tmp/fixture.yml" >/dev/null 2>&1; then
    die 'missing native guest runner step accepted'
  fi
  cp "$workflow" "$tmp/fixture.yml"
  sed -i 's/guest_receipt_protocol=UNSUPPORTED/guest_receipt_protocol=SUPPORTED/g' "$tmp/fixture.yml"
  if bash "$0" "$tmp/fixture.yml" >/dev/null 2>&1; then
    die 'false Java receipt parity accepted'
  fi
  cp "$workflow" "$tmp/fixture.yml"
  sed -i '/ctest --test-dir build --output-on-failure --no-tests=error -R /s/.$//' "$tmp/fixture.yml"
  if bash "$0" "$tmp/fixture.yml" >/dev/null 2>&1; then
    die 'unterminated workflow CI shell quote was accepted'
  fi
  cp "$workflow" "$tmp/fixture.yml"
  sed -i 's/persist-credentials: false/persist-credentials: true/g' "$tmp/fixture.yml"
  if bash "$0" "$tmp/fixture.yml" >/dev/null 2>&1; then
    die 'persistent GitHub checkout credentials accepted'
  fi
  cp "$workflow" "$tmp/fixture.yml"
  sed -i 's/ORES_NO_GC_IR_AUDIT=/DISABLED_NO_GC_IR_AUDIT=/g' "$tmp/fixture.yml"
  if bash "$0" "$tmp/fixture.yml" >/dev/null 2>&1; then
    die 'workflow without mandatory no-GC auditor accepted'
  fi
  cp "$runner" "$tmp/mutated-harness.sh"
  sed -i '/timeout 20s "\$auditor" "\$ir"/d' "$tmp/mutated-harness.sh"
  if ATTO_NATIVE_HARNESS_SCRIPT="$tmp/mutated-harness.sh" bash "$0" "$workflow" >/dev/null 2>&1; then
    die 'source harness bypassed mandatory no-GC auditor'
  fi
  grep -Fvx "      - 'src/main/**'" "$workflow" > "$tmp/fixture.yml"
  if bash "$0" "$tmp/fixture.yml" >/dev/null 2>&1; then
    die 'compiler-source parity workflow trigger removed'
  fi
  grep -Fvx "      - 'CMakeLists.txt'" "$workflow" > "$tmp/fixture.yml"
  if bash "$0" "$tmp/fixture.yml" >/dev/null 2>&1; then
    die 'build-system parity workflow trigger removed'
  fi
fi
echo 'PASS LLVM canonical .atto workflow source-only admission (nine adversarial mutations)'
