#!/usr/bin/env bash
# Source-only M0 gate: compiler-owned std/testing_scalar.atto fidelity.
# Never report this test as LLVM compiler or native guest execution.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
source="${1:-$root/stdlib/testing_scalar.atto}"
header="${2:-$root/src/main/cpp/oreslang/frontend/CoreLibrarySources.hpp}"
loader="${3:-$root/src/main/cpp/oreslang/frontend/ModuleLoader.cpp}"
cases="${4:-$root/tests/atto-native-harness/cases.tsv}"
negatives="${5:-$root/tests/atto-native-harness/negative.tsv}"
die() { printf 'FAIL AttoLang LLVM scalar module source gate: %s\n' "$*" >&2; exit 1; }
for path in "$source" "$header" "$loader" "$cases" "$negatives"; do
  [[ -f "$path" && ! -L "$path" ]] || die "source missing or symlinked: $path"
done
[[ "$(grep -Fc 'R"ATTO_SCALAR(' "$header")" -eq 1 ]] ||
  die 'missing or duplicated inline .atto source opening delimiter'
[[ "$(grep -Fxc ')ATTO_SCALAR";' "$header")" -eq 1 ]] ||
  die 'missing or duplicated inline .atto source closing delimiter'
tmp="$(mktemp -d)"
trap 'rm -rf -- "$tmp"' EXIT
awk '
  /^R"ATTO_SCALAR\(/ {
    inside=1
    sub(/^R"ATTO_SCALAR\(/, "")
    print
    next
  }
  /^\)ATTO_SCALAR";$/ { inside=0; next }
  inside { print }
' "$header" > "$tmp/compiled-source.atto"
cmp -s "$source" "$tmp/compiled-source.atto" ||
  die 'compiled source differs bytewise from canonical .atto file'
grep -Fq '"@ores.std/testing_scalar.atto"' "$header" ||
  die 'compiler registry lacks canonical .atto unit identity'
grep -Fq 'CompilerLibraryTier::Std' "$header" ||
  die 'hosted-only standard library tier missing'
grep -Fq 'is_native_scalar_testing_import(raw)' "$loader" ||
  die 'LLVM loader does not recognize scalar .atto imports'
grep -Fq 'library->tier == frontend::CompilerLibraryTier::Std && !host' "$loader" ||
  die 'LLVM freestanding admission does not reject hosted-only scalar module'
grep -Fq 'native ores.std package resolution remains unsupported' "$loader" ||
  die 'unsupported full TestKit must still fail closed'
for canonical in 'ores.std/testing_scalar' 'ores.std/testing_scalar.atto' \
                 'std/testing_scalar' 'std/testing_scalar.atto'; do
  grep -Fq "raw == \"$canonical\"" "$header" ||
    die "missing canonical compiler-owned alias: $canonical"
done
grep -Fq 'pub fnc equal_int(int actual, int expected): int' "$source" ||
  die 'missing equal_int exported scalar assertion'
grep -Fq 'pub fnc not_equal_int(int actual, int expected): int' "$source" ||
  die 'missing not_equal_int exported scalar assertion'
grep -Fq 'pub fnc combine_status(int first, int second): int' "$source" ||
  die 'missing combined-status assertion'
grep -Fq 'pub fnc greater_than_int(int actual, int lower_bound): int' "$source" ||
  die 'missing strict greater-than assertion'
grep -Fq 'pub fnc less_than_int(int actual, int upper_bound): int' "$source" ||
  die 'missing strict less-than assertion'
grep -Fq 'pub fnc in_range_int(int actual, int lower_bound, int upper_bound): int' "$source" ||
  die 'missing inclusive range assertion'
grep -Fq 'return lower_bound > upper_bound ? 1 : (actual < lower_bound ? 1 : (actual > upper_bound ? 1 : 0));' "$source" ||
  die 'inclusive range must reject inverted bounds and out-of-range values without arithmetic'
for id in std-scalar-equal-pass std-scalar-equal-fail \
          std-scalar-not-equal-pass std-scalar-combined-fail \
          std-scalar-combined-success std-scalar-alias-pass \
          std-scalar-greater-pass std-scalar-greater-fail \
          std-scalar-less-pass std-scalar-less-fail \
          std-scalar-range-lower std-scalar-range-upper \
          std-scalar-range-inverted std-scalar-range-outside \
          std-scalar-greater-equal std-scalar-less-equal; do
  [[ "$(grep -Ec "^$id\\|positive/[^|]+\\.atto\\|[01]$" "$cases")" -eq 1 ]] ||
    die "missing or duplicate native .atto scalar case: $id"
done
for id in std-scalar-legacy-suffix std-scalar-unknown; do
  [[ "$(grep -Ec "^$id\\|negative/[^|]+\\.atto\\|native ores\\.std package resolution remains unsupported$" "$negatives")" -eq 1 ]] ||
    die "missing or duplicate LLVM negative compiler case: $id"
done
if [[ "$#" -eq 0 ]]; then
  cp "$source" "$tmp/source.atto"
  sed -i 's/return actual == expected ? 0 : 1;/return actual == expected ? 1 : 0;/' "$tmp/source.atto"
  if bash "$0" "$tmp/source.atto" "$header" "$loader" "$cases" "$negatives" >/dev/null 2>&1; then
    die 'accepted inverted scalar assertion logic'
  fi

  cp "$source" "$tmp/source.atto"
  sed -i 's/return lower_bound > upper_bound ? 1 :/return lower_bound > upper_bound ? 0 :/' "$tmp/source.atto"
  if bash "$0" "$tmp/source.atto" "$header" "$loader" "$cases" "$negatives" >/dev/null 2>&1; then
    die 'accepted inverted-range assertion downgrade'
  fi

  cp "$header" "$tmp/header.hpp"
  sed -i 's#@ores.std/testing_scalar.atto#@ores.std/testing_scalar.ores#g' "$tmp/header.hpp"
  if bash "$0" "$source" "$tmp/header.hpp" "$loader" "$cases" "$negatives" >/dev/null 2>&1; then
    die 'accepted legacy unit identity'
  fi

  cp "$loader" "$tmp/loader.cpp"
  sed -i 's/native ores.std package resolution remains unsupported/native ores.std unexpectedly accepted/g' "$tmp/loader.cpp"
  if bash "$0" "$source" "$header" "$tmp/loader.cpp" "$cases" "$negatives" >/dev/null 2>&1; then
    die 'accepted removal of unsupported full TestKit boundary'
  fi
fi
echo 'PASS: canonical AttoLang scalar stdlib source mirror, tier, aliases and 4 downgrades (SOURCE_ONLY)'
