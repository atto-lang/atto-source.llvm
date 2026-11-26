#!/usr/bin/env bash
# Real Atto consumer through existing source module linker, followed by
# compiler-IR partitioning. This is NOT interface-only frontend compilation:
# provider source is needed for initial name/type resolution.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
compiler="${ORESLANG_LLVMC:-$root/build/oreslang-llvmc}"
auditor="${ORES_NO_GC_IR_AUDIT:-$root/build/oreslang-no-gc-ir-audit}"
clang="${CLANG:-clang-19}"
extract="${LLVM_EXTRACT:-llvm-extract-19}"
driver="$root/scripts/compile-scalar-module-object.sh"
work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT
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
  "$clang" "-O$opt" -x ir -c "$d/consumer.ll" -o "$d/consumer.o"
  # No provider body in the consumer, and both imports must survive optimization.
  nm --defined-only --format=posix "$d/consumer.o" >"$d/defined"
  ! grep -Fq '__ores_module_' "$d/defined"
  nm --undefined-only --format=posix "$d/consumer.o" | awk '$1 ~ /^__ores_module_/ {print $1}' | sort >"$d/imports"
  printf '%s\n' __ores_module_1.value __ores_module_2.value >"$d/expected"
  cmp "$d/imports" "$d/expected"
  provider() {
    local name="$1" slot="$2" dest="$3"
    bash "$driver" "$work/$name.atto" "$dest.raw" "$name" value
    # Fixture-local symbol translation, not a production namespace ABI.
    objcopy --redefine-sym "$name.value=__ores_module_$slot.value" "$dest.raw" "$dest"
    nm --defined-only --extern-only --format=posix "$dest" |
      awk -v expected="__ores_module_$slot.value" '$1==expected && $2=="T" {n++} END {exit !(NR==1 && n==1)}'
    test -z "$(nm --undefined-only "$dest")"
  }
  provider cart 1 "$d/cart.o"
  provider tax 2 "$d/tax.o"
  sha256sum "$d/consumer.o" "$d/tax.o" >"$d/reuse"
  "$clang" "$d/consumer.o" "$d/cart.o" "$d/tax.o" -o "$d/old"
  rc=0; "$d/old" || rc=$?; test "$rc" = 42
  sed -i 's/return 40/return 41/' "$work/cart.atto"
  provider cart 1 "$d/cart-new.o"
  ! cmp -s "$d/cart.o" "$d/cart-new.o"
  sha256sum -c "$d/reuse"
  # Source-free relink and execution. Missing provider must fail to link.
  mv "$work/cart.atto" "$work/cart.hidden"
  mv "$work/tax.atto" "$work/tax.hidden"
  mv "$work/main.atto" "$work/main.hidden"
  if "$clang" "$d/consumer.o" "$d/cart-new.o" -o "$d/missing" >"$d/missing.log" 2>&1; then exit 1; fi
  grep -Fq '__ores_module_2.value' "$d/missing.log"
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
  printf 'ATTO_MODULE_CONSUMER_PARTITION|O%s|native=42/43/42/43|unchanged=2|missing_provider=REJECTED|initial_source_graph=REQUIRED\n' "$opt"
done
