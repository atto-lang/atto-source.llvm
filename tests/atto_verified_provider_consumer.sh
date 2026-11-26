#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
compiler="${ORESLANG_LLVMC:-$root/build/oreslang-llvmc}"
auditor="${ORES_NO_GC_IR_AUDIT:-$root/build/oreslang-no-gc-ir-audit}"
clang="${CLANG:-clang-19}"
work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT
"${CXX:-g++}" -std=c++17 -O2 -I"$root/src/main/cpp" "$root/tests/native_scalar_slot_fixture.cpp" -o "$work/slot"
cart="$("$work/slot" cart 1 1)"
tax="$("$work/slot" tax 1 1)"
test "$cart" = "$("$work/slot" cart 2 2)"
for opt in 0 2; do
  d="$work/$opt"; mkdir "$d"
  export ORESLANG_LLVM_AOT_OPT_LEVEL="$opt"
  for spec in cart:40 tax:2 cart-new:41; do
    label="${spec%:*}"; val="${spec#*:}"; mod="${label%-new}"
    printf 'define module %s as pub fnc value(): int { return %s; } end\n' "$mod" "$val" >"$d/$label.atto"
    mode=cart1; symbol="$cart"
    if [[ "$label" == cart-new ]]; then mode=cart2; fi
    if [[ "$mod" == tax ]]; then mode=tax1; symbol="$tax"; fi
    "$compiler" --selftest-interface-provider "$mode" "$d/$label.atto" >"$d/$label.ll"
    "$auditor" "$d/$label.ll" >/dev/null
    grep -Fq "define i64 @$symbol()" "$d/$label.ll"
    "$clang" "-O$opt" -x ir -c "$d/$label.ll" -o "$d/$label.o"
    nm --defined-only --extern-only --format=posix "$d/$label.o" |
      awk -v expected="$symbol" '$1==expected && $2=="T" {n++} END {exit !(NR==1 && n==1)}'
    test -z "$(nm --undefined-only "$d/$label.o")"
    if [[ "$label" == cart ]]; then
      for invalid in stale signature; do
        if "$compiler" --selftest-interface-provider "$invalid" "$d/$label.atto" >"$d/denied.ll" 2>"$d/denied.err"; then exit 1; fi
        test ! -s "$d/denied.ll"
        case "$invalid" in
          stale) grep -Fq 'invalid or stale scalar logical slot pin' "$d/denied.err";;
          signature) grep -Fq 'unsupported scalar import signature' "$d/denied.err";;
        esac
      done
      cp "$d/$label.atto" "$d/tampered.atto"
      printf ' ' >>"$d/tampered.atto"
      if "$compiler" --selftest-interface-provider cart1 "$d/tampered.atto" >"$d/denied.ll" 2>"$d/denied.err"; then exit 1; fi
      test ! -s "$d/denied.ll"
      grep -Fq 'verified provider source digest mismatch' "$d/denied.err"
      rm "$d/tampered.atto"
    fi
    rm "$d/$label.atto" "$d/$label.ll"
  done
  test -z "$(find "$d" -name '*.atto' -print)"
  # The actual namespace consumer is parsed in the compiler using only pinned
  # fixture public interface metadata. No provider body or IR extraction.
  "$compiler" --selftest-interface-consumer ok >"$d/consumer.ll"
  "$auditor" "$d/consumer.ll" >/dev/null
  for symbol in "$cart" "$tax"; do
    grep -Fq "declare i64 @$symbol()" "$d/consumer.ll"
    grep -Fq "call i64 @$symbol()" "$d/consumer.ll"
    if grep -Fq "define i64 @$symbol()" "$d/consumer.ll"; then exit 1; fi
  done
  "$clang" "-O$opt" -x ir -c "$d/consumer.ll" -o "$d/consumer.o"
  nm --undefined-only --format=posix "$d/consumer.o" | awk '$1 ~ /^__atto_slot_v1_/ {print $1}' | sort >"$d/imports"
  printf '%s\n' "$cart" "$tax" | sort >"$d/expected"
  cmp "$d/imports" "$d/expected"
  sha256sum "$d/consumer.o" "$d/tax.o" >"$d/reuse"
  "$clang" "$d/consumer.o" "$d/cart.o" "$d/tax.o" -o "$d/old"
  rc=0; "$d/old" || rc=$?; test "$rc" = 42
  "$clang" "$d/consumer.o" "$d/cart-new.o" "$d/tax.o" -o "$d/new"
  rc=0; "$d/new" || rc=$?; test "$rc" = 43
  rc=0; "$d/old" || rc=$?; test "$rc" = 42
  sha256sum -c "$d/reuse"
  for mode in missing arity stale signature; do
    if "$compiler" --selftest-interface-consumer "$mode" >"$d/denied.ll" 2>"$d/denied.err"; then exit 1; fi
    test ! -s "$d/denied.ll"
    case "$mode" in
      missing) grep -Fq 'missing or ambiguous scalar import' "$d/denied.err";;
      arity) grep -Fq 'unsupported interface consumer expression' "$d/denied.err";;
      stale) grep -Fq 'invalid or stale scalar logical slot pin' "$d/denied.err";;
      signature) grep -Fq 'unsupported scalar import signature' "$d/denied.err";;
    esac
  done
  printf 'ATTO_VERIFIED_PROVIDER_CONSUMER|O%s|native=42/43/42|provider_sources=ABSENT|consumer_partition=NO|symbol_rewrite=NO|negative=7\n' "$opt"
done
