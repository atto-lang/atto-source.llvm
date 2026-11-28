#!/usr/bin/env bash
# Real compiler IR partition + fixture-admitted stable logical symbols.
# Still needs provider source for initial frontend resolution. Interface
# metadata and release pins are test fixtures, not production trust.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
compiler="${ORESLANG_LLVMC:-$root/build/oreslang-llvmc}"
auditor="${ORES_NO_GC_IR_AUDIT:-$root/build/oreslang-no-gc-ir-audit}"
clang="${CLANG:-clang-19}"
extract="${LLVM_EXTRACT:-llvm-extract-19}"
driver="$root/scripts/compile-scalar-module-object.sh"
work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT
"${CXX:-g++}" -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror -I"$root/src/main/cpp" "$root/tests/native_scalar_slot_fixture.cpp" -o "$work/slot"
cart_slot="$("$work/slot" cart 1 1)"
tax_slot="$("$work/slot" tax 1 1)"
test "$cart_slot" != "$tax_slot"
test "$cart_slot" = "$("$work/slot" cart 2 2)"
if "$work/slot" cart 2 1 >"$work/stale" 2>&1; then exit 1; fi
grep -Fq 'invalid or stale scalar logical slot pin' "$work/stale"
cat >"$work/main.atto" <<'ATTO'
import module cart from "./cart.atto";
import module tax from "./tax.atto";
pub fnc main(): int { return cart.value() + tax.value(); }
ATTO
printf 'define module cart as pub fnc value(): int { return 40; } end\n' >"$work/cart.atto"
printf 'define module tax as pub fnc value(): int { return 2; } end\n' >"$work/tax.atto"
# The native source linker uses physical unit prefixes in this exact fixture.
# Verify both compiler definitions and actual consumer calls before extracting.
for opt in 0 2; do
  d="$work/$opt"; mkdir "$d"
  export ORESLANG_LLVM_AOT_OPT_LEVEL="$opt"
  "$compiler" --target=host "$work/main.atto" >"$d/full.ll"
  "$auditor" "$d/full.ll" >/dev/null
  for symbol in __ores_module_1.value __ores_module_2.value; do
    grep -Fq "define i64 @$symbol()" "$d/full.ll"
    grep -Fq "call i64 @$symbol()" "$d/full.ll"
  done
  "$extract" --delete --func=__ores_module_1.value --func=__ores_module_2.value -S "$d/full.ll" -o "$d/consumer.ll"
  "$auditor" "$d/consumer.ll" >/dev/null
  "$clang" "-O$opt" -x ir -c "$d/consumer.ll" -o "$d/consumer.raw.o"
  objcopy --redefine-sym "__ores_module_1.value=$cart_slot" --redefine-sym "__ores_module_2.value=$tax_slot" "$d/consumer.raw.o" "$d/consumer.o"
  # No provider body in the consumer, and both imports must survive optimization.
  nm --defined-only --format=posix "$d/consumer.o" >"$d/defined"
  if grep -Eq '__ores_module_|__atto_slot_v1_' "$d/defined"; then echo 'provider definition leaked into consumer' >&2; exit 1; fi
  nm --undefined-only --format=posix "$d/consumer.o" | awk '$1 ~ /^__atto_slot_v1_/ {print $1}' | sort >"$d/imports"
  printf '%s\n' "$cart_slot" "$tax_slot" | sort >"$d/expected"
  cmp "$d/imports" "$d/expected"
  provider() {
    local name="$1" release="$2" dest="$3" symbol
    symbol="$("$work/slot" "$name" "$release" "$release")"
    bash "$driver" "$work/$name.atto" "$dest.raw" "$name" value
    # Fixture-only mapping from compiler output to admitted logical symbol.
    objcopy --redefine-sym "$name.value=$symbol" "$dest.raw" "$dest"
    nm --defined-only --extern-only --format=posix "$dest" |
      awk -v expected="$symbol" '$1==expected && $2=="T" {n++} END {exit !(NR==1 && n==1)}'
    test -z "$(nm --undefined-only "$dest")"
  }
  provider cart 1 "$d/cart.o"
  provider tax 1 "$d/tax.o"
  sha256sum "$d/consumer.o" "$d/tax.o" >"$d/reuse"
  "$clang" "$d/consumer.o" "$d/cart.o" "$d/tax.o" -o "$d/old"
  rc=0; "$d/old" || rc=$?; test "$rc" = 42
  sed -i 's/return 40/return 41/' "$work/cart.atto"
  provider cart 2 "$d/cart-new.o"
  if cmp -s "$d/cart.o" "$d/cart-new.o"; then echo "provider edit did not change object" >&2; exit 1; fi
  sha256sum -c "$d/reuse"
  # Source-free relink and execution. Missing provider must fail to link.
  mv "$work/cart.atto" "$work/cart.hidden"
  mv "$work/tax.atto" "$work/tax.hidden"
  mv "$work/main.atto" "$work/main.hidden"
  if "$clang" "$d/consumer.o" "$d/cart-new.o" -o "$d/missing" >"$d/missing.log" 2>&1; then exit 1; fi
  grep -Fq "$tax_slot" "$d/missing.log"
  # The fixture's expected digest is captured from the compiler-produced object,
  # never recomputed from a replacement candidate before admission.
  sha256sum "$d/cart-new.o" >"$d/provider-pin"
  cp "$d/cart-new.o" "$d/tampered.o"
  printf x >>"$d/tampered.o"
  expected_digest="$(cut -d' ' -f1 "$d/provider-pin")"
  if test "$(sha256sum "$d/tampered.o" | cut -d' ' -f1)" = "$expected_digest"; then exit 1; fi
  sha256sum -c "$d/provider-pin"
  "$clang" "$d/consumer.o" "$d/cart-new.o" "$d/tax.o" -o "$d/new"
  rc=0; "$d/new" || rc=$?; test "$rc" = 43
  rc=0; "$d/old" || rc=$?; test "$rc" = 42
  mv "$work/cart.hidden" "$work/cart.atto"
  mv "$work/tax.hidden" "$work/tax.atto"
  mv "$work/main.hidden" "$work/main.atto"
  "$compiler" --target=host "$work/main.atto" >"$d/clean.ll"
  "$auditor" "$d/clean.ll" >/dev/null
  "$clang" "-O$opt" "$d/clean.ll" -o "$d/clean"
  rc=0; "$d/clean" || rc=$?; test "$rc" = 43
  sha256sum -c "$d/reuse"
  sed -i 's/return 41/return 40/' "$work/cart.atto"
  printf 'ATTO_VERIFIED_SLOT_RELINK|O%s|native=42/43/42/43|unchanged=2|stale_pin=REJECTED|initial_source_graph=REQUIRED|fixture_metadata=YES\n' "$opt"
done
