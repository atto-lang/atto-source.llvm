#!/usr/bin/env bash
# Real native compiler -> LLVM IR -> Clang -> executable AttoLang guest gate.
# A source fixture, compiler-only acceptance, or host-model result is NOT a pass.
set -euo pipefail
if (( $# != 3 )); then
  echo "usage: $0 ORESLANG_LLVMC CLANG FIXTURE_DIR" >&2
  exit 64
fi
compiler="$1"
clang="$2"
fixtures="$3"
fail() { echo "ATTO_FIRST_LLVM_FAIL: $*" >&2; exit 1; }
[[ -x "$compiler" ]] || fail "real compiler unavailable: $compiler"
command -v "$clang" >/dev/null 2>&1 || fail "Clang unavailable: $clang"
for name in main.atto math.atto explicit.atto; do
  [[ -f "$fixtures/$name" ]] || fail "missing checked-in .atto fixture: $name"
done

# Pin the exact shared Java/LLVM AttoLang fixture blob IDs before execution.
command -v git >/dev/null 2>&1 || fail "Git required for pinned fixture identity"
for pin in \
    main.atto:f152f9b46014975f471b827c82e8fe28107f9a0e \
    math.atto:01aa7b955eb3e69d9714c4a6893b6f900e493097 \
    explicit.atto:f9502fb537f0a6b1bf2fd000f45ca2163bd9d30f; do
  IFS=: read -r name expected <<< "$pin"
  observed="$(git hash-object -- "$fixtures/$name")" ||
    fail "could not hash checked-in fixture: $name"
  [[ "$observed" == "$expected" ]] ||
    fail "shared Java/LLVM fixture blob mismatch: $name"
done

work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT
new_case() {
  local label="$1" source="$2"
  mkdir -p "$work/$label"
  cp -- "$fixtures/$source" "$work/$label/main.atto"
  cp -- "$fixtures/math.atto" "$work/$label/math.atto"
}
compile_positive() {
  local label="$1"
  local case_dir="$work/$label"
  local ir="$case_dir/main.ll"
  "$compiler" --target=host "$case_dir/main.atto" > "$ir" 2> "$case_dir/compiler.err" ||
    fail "$label: compiler rejected canonical .atto source"
  [[ -s "$ir" ]] || fail "$label: compiler returned success without LLVM IR"
  grep -Fq '@main(' "$ir" || fail "$label: no compiled native main entry in LLVM IR"
  for opt in 0 2; do
    "$clang" "-O$opt" -x ir "$ir" -o "$case_dir/program-O$opt" 2> "$case_dir/clang-O$opt.err" ||
      fail "$label: Clang -O$opt failed"
    local code=0
    "$case_dir/program-O$opt" > "$case_dir/stdout-O$opt" 2> "$case_dir/stderr-O$opt" || code=$?
    [[ "$code" -eq 42 ]] || fail "$label: native guest -O$opt returned $code instead of 42"
    [[ ! -s "$case_dir/stdout-O$opt" && ! -s "$case_dir/stderr-O$opt" ]] ||
      fail "$label: unexpected native guest stdout/stderr at -O$opt"
    echo "ATTO_FIRST_LLVM_EXECUTED=$label:-O$opt:exit42"
  done
}
compile_negative() {
  local label="$1"
  local case_dir="$work/$label"
  local ir="$case_dir/main.ll"
  if "$compiler" --target=host "$case_dir/main.atto" > "$ir" 2> "$case_dir/compiler.err"; then
    fail "$label: invalid imported .atto source compiled successfully"
  fi
  [[ ! -s "$ir" ]] || fail "$label: rejected .atto emitted partial/executable LLVM IR"
  [[ -s "$case_dir/compiler.err" ]] || fail "$label: rejected .atto emitted no diagnostic"
  echo "ATTO_FIRST_LLVM_REJECTED=$label:NO_IR"
}

new_case relative_main main.atto
compile_positive relative_main

# An explicit modern source must never be replaced by a legacy same-stem file.
new_case explicit_modern_with_legacy_shadow explicit.atto
cat > "$work/explicit_modern_with_legacy_shadow/math.ores" <<'ORES'
define module Math as
  pub fnc add(int n): int { return 17; }
end
ORES
compile_positive explicit_modern_with_legacy_shadow

# Java parity: an extensionless import must prefer .atto when a legacy .ores
# file with different guest behavior is also present.
new_case implicit_modern_with_legacy_shadow main.atto
cat > "$work/implicit_modern_with_legacy_shadow/math.ores" <<'ORES'
define module Math as
  pub fnc add(int n): int { return 17; }
end
ORES
compile_positive implicit_modern_with_legacy_shadow

# Java compatibility: absent modern file permits a legacy .ores migration
# dependency. The guest implementation remains authored in AttoLang.
new_case legacy_only_migration_fallback main.atto
mv -- "$work/legacy_only_migration_fallback/math.atto" \
  "$work/legacy_only_migration_fallback/math.ores"
compile_positive legacy_only_migration_fallback

new_case missing_dependency main.atto
rm -- "$work/missing_dependency/math.atto"
compile_negative missing_dependency

new_case invalid_dependency main.atto
printf 'define module Math as pub fnc answer(): int { return missing_name; } end\n' \
  > "$work/invalid_dependency/math.atto"
compile_negative invalid_dependency

# Directory masquerading as canonical source must fail rather than select a
# legacy fallback (or read arbitrary nonregular bytes).
new_case nonregular_canonical main.atto
rm -- "$work/nonregular_canonical/math.atto"
mkdir "$work/nonregular_canonical/math.atto"
cat > "$work/nonregular_canonical/math.ores" <<'ORES'
define module Math as
  pub fnc add(int n): int { return 17; }
end
ORES
compile_negative nonregular_canonical

# Canonical presence includes dangling symlink directory entries; selecting
# the shadowing .ores or chasing the symlink would be unsafe.
if [[ "$(uname -s)" != "MINGW"* && "$(uname -s)" != "MSYS"* ]]; then
  new_case dangling_modern_with_legacy_shadow main.atto
  rm -- "$work/dangling_modern_with_legacy_shadow/math.atto"
  ln -s "missing-canonical.atto" "$work/dangling_modern_with_legacy_shadow/math.atto"
  cp -- "$fixtures/math.atto" "$work/dangling_modern_with_legacy_shadow/math.ores"
  compile_negative dangling_modern_with_legacy_shadow
  echo "ATTO_FIRST_LLVM_AOT_GATE=8_EXECUTED_4_REJECTED"
else
  echo "ATTO_FIRST_LLVM_AOT_GATE=8_EXECUTED_3_REJECTED_NO_SYMLINK_TEST"
fi
