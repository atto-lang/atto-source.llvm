#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
compiler="${ORESLANG_LLVMC:-$root/build/oreslang-llvmc}"
auditor="${ORES_NO_GC_IR_AUDIT:-$root/build/oreslang-no-gc-ir-audit}"
clang="${CLANG:-clang-19}"
work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT
"${CXX:-g++}" -std=c++17 -O2 -I"$root/src/main/cpp" "$root/tests/native_scalar_slot_fixture.cpp" -o "$work/slot"
"${CXX:-g++}" -std=c++17 -O2 -Wall -Wextra -Werror -I"$root/src/main/cpp" "$root/tests/native_scalar_object_admission_fixture.cpp" -o "$work/admit"
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
    release=1; if [[ "$label" == cart-new ]]; then release=2; fi
    source_pin="$(sha256sum "$d/$label.atto" | cut -d' ' -f1)"
    object_pin="$(sha256sum "$d/$label.o" | cut -d' ' -f1)"
    # Fixture host records the independently compiled original, not the candidate.
    mv "$d/$label.o" "$d/$label.candidate"
    admit() {
      "$work/admit" "$mod" "$release" "$release" "$source_pin" "$object_pin" x86_64-pc-linux-gnu "O$opt" "$1" "$2"
    }
    admit "$d/$label.candidate" ok >"$d/$label.o"
    cmp "$d/$label.candidate" "$d/$label.o"
    if [[ "$label" == cart ]]; then
      for invalid in symbol source target profile; do
        if admit "$d/$label.candidate" "$invalid" >"$d/denied.o" 2>"$d/denied.err"; then exit 1; fi
        test ! -s "$d/denied.o"
      done
      cp "$d/$label.candidate" "$d/tampered.o"
      printf x >>"$d/tampered.o"
      if admit "$d/tampered.o" ok >"$d/denied.o" 2>"$d/denied.err"; then exit 1; fi
      test ! -s "$d/denied.o"
      grep -Fq 'native object bytes digest mismatch' "$d/denied.err"
      if "$work/admit" "$mod" "$release" 2 "$source_pin" "$object_pin" x86_64-pc-linux-gnu "O$opt" "$d/$label.candidate" ok >"$d/denied.o" 2>"$d/denied.err"; then exit 1; fi
      test ! -s "$d/denied.o"
      # Test-owned rehash isolates structural validation from digest rejection.
      # These are malformed artifacts, never submitted to the native linker.
      deny_structure() {
        local bad="$1" bad_digest
        bad_digest="$(sha256sum "$bad" | cut -d' ' -f1)"
        if "$work/admit" "$mod" "$release" "$release" "$source_pin" "$bad_digest" x86_64-pc-linux-gnu "O$opt" "$bad" ok >"$d/denied.o" 2>"$d/denied.err"; then exit 1; fi
        test ! -s "$d/denied.o"
        grep -Fq 'restricted ELF scalar object rejected' "$d/denied.err"
      }
      cp "$d/$label.candidate" "$d/bad.o"
      printf '\\000' | dd of="$d/bad.o" bs=1 seek=0 conv=notrunc status=none
      deny_structure "$d/bad.o"
      cp "$d/$label.candidate" "$d/bad.o"
      printf '\\002\\000' | dd of="$d/bad.o" bs=1 seek=16 conv=notrunc status=none
      deny_structure "$d/bad.o"
      cp "$d/$label.candidate" "$d/bad.o"
      printf '\\267\\000' | dd of="$d/bad.o" bs=1 seek=18 conv=notrunc status=none
      deny_structure "$d/bad.o"
      head -c 64 "$d/$label.candidate" >"$d/bad.o"
      deny_structure "$d/bad.o"
      cp "$d/$label.candidate" "$d/bad.o"
      printf '\\377\\377' | dd of="$d/bad.o" bs=1 seek=60 conv=notrunc status=none
      deny_structure "$d/bad.o"
      # Rehash a real but different exported symbol. A valid ELF and matching
      # object digest must not authorize the wrong physical slot.
      objcopy --redefine-sym "$cart=wrong_provider" "$d/$label.candidate" "$d/bad.o"
      deny_structure "$d/bad.o"
      rm "$d/bad.o"
    fi
    rm "$d/$label.candidate"
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
  printf 'ATTO_STRUCTURAL_NATIVE_OBJECTS|O%s|native=42/43/42|immutable_snapshot=YES|positive_symbol_rewrite=NO|structure_negative=6\n' "$opt"
done
