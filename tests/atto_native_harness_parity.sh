#!/usr/bin/env bash
# Real LLVM compiler -> LLVM IR -> Clang O0/O2 -> native OS-exit test harness.
# These are guest executions for the portable AttoLang scalar subset, NOT
# TestCase/RunSummary/ORES_TEST runtime parity with the Java standard library.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
corpus="$root/tests/atto-native-harness"
manifest="${ATTO_NATIVE_PARITY_MANIFEST:-$corpus/cases.tsv}"
negative_manifest="${ATTO_NATIVE_NEGATIVE_MANIFEST:-$corpus/negative.tsv}"
compiler="${ORESLANG_LLVMC:-$root/build/oreslang-llvmc}"
clang="${CLANG:-clang}"
mode="${1:---run}"

die() { printf 'FAIL AttoLang LLVM native guest parity: %s\n' "$*" >&2; exit 1; }
[[ "$mode" == --run || "$mode" == --inventory-only ]] ||
  die 'expected --run or --inventory-only'
[[ -f "$manifest" && ! -L "$manifest" && -f "$negative_manifest" && ! -L "$negative_manifest" ]] ||
  die 'missing or symlinked trusted test manifests'
[[ "$manifest" != "$negative_manifest" ]] || die 'manifests alias each other'
[[ -d "$corpus" && ! -L "$corpus" &&
   -d "$corpus/positive" && ! -L "$corpus/positive" &&
   -d "$corpus/negative" && ! -L "$corpus/negative" ]] ||
  die 'symlinked native parity source directory'
declare -A seen=()
declare -a positive_ids=() positive_paths=() expected_exits=()
declare -a negative_ids=() negative_paths=() diagnostic_needles=()
while IFS='|' read -r id path expected extra || [[ -n "${id:-}" ]]; do
  [[ "$id" == \#* || -z "$id" ]] && continue
  [[ -z "${extra:-}" && "$id" =~ ^[a-z][a-z0-9-]*$ &&
     "$path" =~ ^positive/[a-z0-9-]+\.atto$ &&
     "$expected" =~ ^(0|[1-9][0-9]{0,2})$ ]] ||
    die "invalid positive manifest entry: $id"
  (( 10#$expected <= 255 )) || die "exit status exceeds OS byte: $id"
  [[ -z "${seen["$id"]+yes}" ]] || die "duplicate test identity $id"
  seen["$id"]=1
  [[ -f "$corpus/$path" && ! -L "$corpus/$path" ]] ||
    die "missing or symlinked .atto source: $id"
  positive_ids+=("$id"); positive_paths+=("$path"); expected_exits+=("$expected")
done < "$manifest"
[[ "${#positive_ids[@]}" -eq 40 ]] || die "expected 40 admitted native scalar cases"

while IFS='|' read -r id path needle extra || [[ -n "${id:-}" ]]; do
  [[ "$id" == \#* || -z "$id" ]] && continue
  [[ -z "${extra:-}" && "$id" =~ ^[a-z][a-z0-9-]*$ &&
     "$path" =~ ^negative/[a-z0-9-]+\.atto$ && -n "$needle" ]] ||
    die "invalid negative manifest entry: $id"
  [[ -z "${seen["$id"]+yes}" ]] || die "duplicate negative identity $id"
  seen["$id"]=1
  [[ -f "$corpus/$path" && ! -L "$corpus/$path" ]] ||
    die "missing or symlinked negative .atto source: $id"
  negative_ids+=("$id"); negative_paths+=("$path"); diagnostic_needles+=("$needle")
done < "$negative_manifest"
[[ "${#negative_ids[@]}" -eq 6 ]] || die 'expected exactly six negative fixtures'
[[ -f "$corpus/positive/.atto.pkg.toml" && ! -L "$corpus/positive/.atto.pkg.toml" ]] ||
  die 'canonical package manifest is absent or symlinked'
[[ -f "$corpus/positive/scalar-assert-lib.atto" && ! -L "$corpus/positive/scalar-assert-lib.atto" ]] ||
  die 'missing native .atto scalar assertion library'
[[ -f "$corpus/positive/module-helper.atto" &&
   ! -L "$corpus/positive/module-helper.atto" ]] || die 'missing .atto module dependency'
for path in "$manifest" "$negative_manifest" \
    "$corpus/positive/.atto.pkg.toml" \
    "$corpus/positive/scalar-assert-lib.atto" \
    "$corpus/positive/module-helper.atto" \
    "${positive_paths[@]/#/$corpus/}" "${negative_paths[@]/#/$corpus/}"; do
  [[ -f "$path" && ! -L "$path" ]] || die "nonregular source: $path"
done

# Reviewed 40-positive / 6-source-negative mappings are frozen by Git blob.
# Explicitly update the hashes when changing a reviewed parity contract.
command -v git >/dev/null || die 'git hash-object required for pinned manifests'
[[ "$(git hash-object -- "$manifest")" == "f632b25781b55f04dfd0519b25089410333edc51" ]] ||
  die 'positive 40-case contract manifest was substituted'
[[ "$(git hash-object -- "$negative_manifest")" == "7c5545448b7011bfddb77be09607f9fef2cb734f" ]] ||
  die 'negative six-case manifest was substituted'

if [[ "$mode" == --inventory-only ]]; then
  echo 'ATTO_LLVM_NATIVE_INVENTORY_ONLY positive=40 negative=6 (NO_COMPILER_EXECUTION)'
  exit 0
fi
[[ -x "$compiler" && ! -L "$compiler" ]] || die "missing non-symlink executable compiler: $compiler"
# Every emitted native IR must pass the production no-GC screen before Clang.
auditor="${ORES_NO_GC_IR_AUDIT:-$root/build/oreslang-no-gc-ir-audit}"
[[ -x "$auditor" && ! -L "$auditor" ]] || die "missing native no-GC IR auditor: $auditor"
clang_exe="$(command -v "$clang")" || die "missing Clang"
[[ -x "$clang_exe" ]] || die 'Clang is not executable'
command -v timeout >/dev/null || die 'timeout required'
command -v sha256sum >/dev/null || die 'sha256sum required'
tmp="$(mktemp -d)"
trap 'rm -rf -- "$tmp"' EXIT
declare -A original_hash=()
for path in "$manifest" "$negative_manifest" \
    "$corpus/positive/.atto.pkg.toml" \
    "$corpus/positive/scalar-assert-lib.atto" \
    "$corpus/positive/module-helper.atto" \
    "${positive_paths[@]/#/$corpus/}" "${negative_paths[@]/#/$corpus/}"; do
  original_hash["$path"]="$(sha256sum "$path" | cut -d ' ' -f1)"
done

for idx in "${!positive_ids[@]}"; do
  id="${positive_ids[$idx]}"; path="$corpus/${positive_paths[$idx]}"
  expected="${expected_exits[$idx]}"
  for level in 0 2; do
    ir="$tmp/$id-O$level.ll"; exe="$tmp/$id-O$level.exe"
    set +e
    timeout 60s "$compiler" --target=host "$path" > "$ir" 2> "$tmp/compile.err"
    compiled=$?
    set -e
    [[ "$compiled" -eq 0 && -s "$ir" ]] ||
      { head -c 4096 "$tmp/compile.err" >&2; die "compiler did not produce admitted IR: $id O$level exit=$compiled"; }
    grep -Eq 'define[[:space:]]+i32[[:space:]]+@main\(' "$ir" ||
      die "compiler produced IR without a native main: $id O$level"
    timeout 20s "$auditor" "$ir" >/dev/null 2> "$tmp/audit-$id-O$level.err" || {
      head -c 4096 "$tmp/audit-$id-O$level.err" >&2
      die "mandatory native no-GC IR audit rejected guest: $id O$level"
    }
    timeout 60s "$clang_exe" "-O$level" -x ir "$ir" -o "$exe" \
      > "$tmp/clang.stdout" 2> "$tmp/clang.err" ||
      { head -c 4096 "$tmp/clang.err" >&2; die "LLVM IR did not link: $id O$level"; }
    [[ -f "$exe" && -x "$exe" && ! -L "$exe" ]] || die "native executable absent: $id O$level"
    set +e
    timeout 10s "$exe" > "$tmp/guest.stdout" 2> "$tmp/guest.stderr"
    status=$?
    set -e
    [[ "$status" -eq "$expected" && "$status" -ne 124 && "$status" -ne 137 ]] ||
      die "wrong native guest OS result: $id O$level expected=$expected got=$status"
    [[ ! -s "$tmp/guest.stdout" && ! -s "$tmp/guest.stderr" ]] ||
      die "unexpected guest stdout/stderr: $id O$level"
  done
  cmp -s "$tmp/$id-O0.ll" "$tmp/$id-O2.ll" ||
    die "compiler IR changed between optimization-mode invocations: $id"
  # Only publish per-level success after O0/O2 compilation, no-GC audit,
  # native guest exits and IR equality ALL succeeded.
  for level in 0 2; do
    # Host-observed, not a guest-generated ORES_TEST receipt.
    printf 'ATTO_LLVM_NATIVE_HOST_OBSERVED|%s|O%s|exit=%s|compiler=real|clang=real|no_gc_ir_audit=passed|guest=executed\n' \
      "$id" "$level" "$expected"
  done
done

# Fail-closed source admission: nonzero status alone cannot count as a
# rejection if the compiler emitted any partial IR, or was killed by timeout.
for idx in "${!negative_ids[@]}"; do
  id="${negative_ids[$idx]}"; path="$corpus/${negative_paths[$idx]}"
  ir="$tmp/negative-$id.ll"
  set +e
  timeout 30s "$compiler" --target=host "$path" > "$ir" 2> "$tmp/negative-$id.stderr"
  status=$?
  set -e
  [[ "$status" -ne 0 && "$status" -ne 124 && "$status" -ne 137 && ! -s "$ir" ]] ||
    die "invalid negative admission result: $id status=$status"
  grep -Fq "${diagnostic_needles[$idx]}" "$tmp/negative-$id.stderr" ||
    die "unexpected native compiler rejection diagnostic: $id"
  printf 'ATTO_LLVM_NEGATIVE_HOST_OBSERVED|%s|rejected=1|partial_ir=0\n' "$id"
done

# A canonical .atto importer cannot implicitly load a legacy-only .ores file.
# The .ores file is temporary test input, not new canonical library source.
mkdir "$tmp/no-downgrade"
printf '%s\n' 'import module Legacy from "./legacy"; pub fnc main(): int { return Legacy.answer(); }' \
  > "$tmp/no-downgrade/main.atto"
printf '%s\n' 'define module Legacy as pub fnc answer(): int { return 42; } end' \
  > "$tmp/no-downgrade/legacy.ores"
set +e
timeout 30s "$compiler" --target=host "$tmp/no-downgrade/main.atto" \
  > "$tmp/no-downgrade.ll" 2> "$tmp/no-downgrade.stderr"
status=$?
set -e
[[ "$status" -ne 0 && "$status" -ne 124 && "$status" -ne 137 && ! -s "$tmp/no-downgrade.ll" ]] ||
  die 'LLVM silently admitted an extensionless legacy-only dependency'
grep -Fq 'cannot resolve native import' "$tmp/no-downgrade.stderr" ||
  die 'missing stable canonical-legacy rejection reason'

# A dangling canonical .atto symlink must not redirect to its legacy peer.
mkdir "$tmp/dangling"
printf '%s\n' 'import module Legacy from "./legacy"; pub fnc main(): int { return Legacy.answer(); }' \
  > "$tmp/dangling/main.atto"
printf '%s\n' 'define module Legacy as pub fnc answer(): int { return 42; } end' \
  > "$tmp/dangling/legacy.ores"
ln -s absent-atto "$tmp/dangling/legacy.atto"
set +e
timeout 30s "$compiler" --target=host "$tmp/dangling/main.atto" \
  > "$tmp/dangling.ll" 2> "$tmp/dangling.stderr"
status=$?
set -e
[[ "$status" -ne 0 && "$status" -ne 124 && "$status" -ne 137 && ! -s "$tmp/dangling.ll" ]] ||
  die 'LLVM accepted dangling canonical .atto symlink or downgraded to .ores'

# The Java native test harness is not yet implemented in LLVM stdlib.
# Make the unsupported boundary an explicit negative compiler admission,
# not a fake passing 'ORES_TEST' line or a host-only canary.
mkdir "$tmp/unsupported-stdlib"
printf '%s\n' 'import module Test from "ores.std/testing"; pub fnc main(): int { return 42; }' \
  > "$tmp/unsupported-stdlib/main.atto"
set +e
timeout 30s "$compiler" --target=host "$tmp/unsupported-stdlib/main.atto" \
  > "$tmp/unsupported-stdlib.ll" 2> "$tmp/unsupported-stdlib.stderr"
status=$?
set -e
[[ "$status" -ne 0 && "$status" -ne 124 && "$status" -ne 137 &&
   ! -s "$tmp/unsupported-stdlib.ll" ]] ||
  die 'LLVM falsely admitted unsupported std/testing as executable guest parity'
grep -Fq 'native ores.std package resolution remains unsupported' \
  "$tmp/unsupported-stdlib.stderr" ||
  die 'LLVM std/testing unsupported boundary lacks explicit diagnostic'

# A wasm/freestanding module must never import hosted scalar std/testing,
# even when its current helper implementations are effect-free.
mkdir "$tmp/reject-freestanding-std"
printf '%s\n' 'import module Test from "ores.std/testing/scalar"; pub fnc main(): int { return Test.expect_true(true); }' \
  > "$tmp/reject-freestanding-std/main.atto"
set +e
timeout 30s "$compiler" --target=wasm32-unknown-unknown --library-profile=core \
  "$tmp/reject-freestanding-std/main.atto" > "$tmp/reject-freestanding-std.ll" \
  2> "$tmp/reject-freestanding-std.stderr"
status=$?
set -e
[[ "$status" -ne 0 && "$status" -ne 124 && "$status" -ne 137 &&
   ! -s "$tmp/reject-freestanding-std.ll" ]] ||
  die 'LLVM admitted hosted scalar testing into freestanding wasm'
grep -Fq 'native ores.std testing scalar requires hosted target' \
  "$tmp/reject-freestanding-std.stderr" ||
  die 'freestanding std/testing scalar rejection diagnostic missing'

for path in "${!original_hash[@]}"; do
  [[ "$(sha256sum "$path" | cut -d ' ' -f1)" == "${original_hash["$path"]}" ]] ||
    die "guest source/manifest mutated during compiler execution: $path"
done

# Two one-case guest ORES_TEST smokes are a separate gate (not TestSuite parity).
ORESLANG_LLVMC="$compiler" CLANG="$clang_exe" ORES_NO_GC_IR_AUDIT="$auditor" \
  bash "$root/tests/atto_native_guest_receipts.sh"
printf 'ATTO_LLVM_NATIVE_PARITY_SUBSET_EXECUTED|positive=40|optimizations=2|negative=10|guest_receipt_protocol=UNSUPPORTED\n'
