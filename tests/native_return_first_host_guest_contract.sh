#!/bin/sh
# Real .atto source -> native LLVM IR -> Clang -> executable exit-value gate.
# Do not count this as a Wasm guest, all-backend parity, or CI runner admission.
set -eu
compiler=${1:?expected production oreslang-llvmc}
clang=${2:?expected Clang driver}
test -f "$compiler" && test -x "$compiler" && test ! -L "$compiler"
# CMake's find_program commonly finds a symlink (e.g. clang -> clang-17).
# Reject missing/nonexecutable toolchains, not legitimate version symlinks.
test -f "$clang" && test -x "$clang"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM
guests=0
optimized=0
for header in 'int <- main()' 'int• main()' 'fnc int <- main()' 'fnc int• main()'; do
  for form in braced begin_inline begin_multiline bare bare_semicolon bare_dot; do
    case "$form" in
      braced) printf '%s { return 42; }\n' "$header" ;;
      begin_inline) printf '%s begin return 42; end\n' "$header" ;;
      begin_multiline) printf '%s\nbegin\n  return 42;\nend\n' "$header" ;;
      bare) printf '%s return 42; end\n' "$header" ;;
      bare_semicolon) printf '%s return 42; end;\n' "$header" ;;
      bare_dot) printf '%s\nbegin\n  return 42;\nend.\n' "$header" ;;
    esac > "$work/guest.atto"
    if ! "$compiler" --target=host "$work/guest.atto" >"$work/guest.ll" 2>"$work/compiler.err"; then
      printf 'return-first host IR rejected header=%s form=%s\n' "$header" "$form" >&2
      cat "$work/compiler.err" >&2
      exit 1
    fi
    test -s "$work/guest.ll" || {
      printf 'return-first generated empty host IR: %s / %s\n' "$header" "$form" >&2
      exit 1
    }
    # Validate the host process entrypoint, not an unrelated helper definition.
    grep -Eq '^[[:space:]]*define[[:space:]]+i32[[:space:]]+@main\(' "$work/guest.ll" || {
      printf 'return-first produced no real i32 main entrypoint: %s / %s\n' "$header" "$form" >&2
      exit 1
    }
    "$clang" -O0 -x ir "$work/guest.ll" -o "$work/guest"
    if "$work/guest" >"$work/stdout" 2>"$work/stderr"; then status=0; else status=$?; fi
    if [ "$status" -ne 42 ] || [ -s "$work/stdout" ] || [ -s "$work/stderr" ]; then
      printf 'real guest mismatch header=%s form=%s status=%s (expected 42)\n' "$header" "$form" "$status" >&2
      exit 1
    fi
    guests=$((guests+1))
    # Probe the optimizer separately for one representative body per marker.
    if [ "$form" = begin_inline ]; then
      "$clang" -O2 -x ir "$work/guest.ll" -o "$work/guest-o2"
      if "$work/guest-o2" >"$work/stdout-o2" 2>"$work/stderr-o2"; then status=0; else status=$?; fi
      if [ "$status" -ne 42 ] || [ -s "$work/stdout-o2" ] || [ -s "$work/stderr-o2" ]; then
        printf 'optimized guest mismatch header=%s status=%s\n' "$header" "$status" >&2
        exit 1
      fi
      optimized=$((optimized+1))
    fi
  done
done
# A source mutation must change the observed OS process status. This defeats
# compilers or test harnesses that replace every valid program with exit 42.
printf 'fnc int <- main() { return 43; }\n' >"$work/mutant.atto"
"$compiler" --target=host "$work/mutant.atto" >"$work/mutant.ll" 2>"$work/mutant.err"
test -s "$work/mutant.ll"
"$clang" -O2 -x ir "$work/mutant.ll" -o "$work/mutant"
if "$work/mutant" >"$work/mutant.stdout" 2>"$work/mutant.stderr"; then status=0; else status=$?; fi
if [ "$status" -ne 43 ] || [ -s "$work/mutant.stdout" ] || [ -s "$work/mutant.stderr" ]; then
  printf 'source mutation did not change guest exit to 43 (got %s)\n' "$status" >&2
  exit 1
fi
test "$guests" -eq 24 && test "$optimized" -eq 4
printf 'RETURN_FIRST_NATIVE_GUEST_EXIT_42=%s OPTIMIZED_O2=%s SOURCE_MUTATION_EXIT_43=1 WASM_GUEST_NOT_EXECUTED\n' "$guests" "$optimized"
