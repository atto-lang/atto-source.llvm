#!/bin/sh
# Production native LLVM compiler parser gate. IR output is NOT a guest receipt.
set -eu
compiler=$1
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM
for header in 'int <- main()' 'int• main()' 'fnc int <- main()' 'fnc int• main()'; do
  for closer in end 'end.' 'end;'; do
    { printf '%s\nbegin\n  return 42;\n%s\n' "$header" "$closer"; } > "$work/positive.atto"
    "$compiler" --target=wasm32 "$work/positive.atto" > "$work/out.ll" 2>"$work/error"
    grep -q 'define ' "$work/out.ll" || {
      echo "positive return-first produced no LLVM IR: $header $closer" >&2
      cat "$work/error" >&2
      exit 1
    }
  done
done
for case in 1 2 3 4 5; do
  case "$case" in
    1) printf 'int <- main() begin return 42; end\n' ;;
    2) printf 'int <- main()\nbegin return 42;\nend\n' ;;
    3) printf 'int <- main()\nbegin\nreturn 42;\n' ;;
    4) printf 'int <- main()\nbegin\nreturn 42;\nend..\n' ;;
    5) printf 'fnc int <- main() -> int\nbegin\nreturn 42;\nend\n' ;;
  esac > "$work/negative.atto"
  if "$compiler" --target=wasm32 "$work/negative.atto" > "$work/out.ll" 2>"$work/error"; then
    echo "malformed return-first accepted: $case" >&2
    exit 1
  fi
  if [ -s "$work/out.ll" ]; then
    echo "malformed return-first leaked LLVM IR: $case" >&2
    exit 1
  fi
done
# The class-method grammar is distinct from root functions. Frontend-only
# admission confirms all four spellings bind the receiver-bearing method path
# without claiming Wasm class object ABI or guest execution.
for marker in '' 'mtd ' 'fnc '; do
  for separator in '<-' '•'; do
    { printf 'define class Box as\n  %sint %s value()\n  begin\n    return 42;\n  end.\nend\n' "$marker" "$separator"
      printf 'fnc main(): int { return 42; }\n'
    } > "$work/method.atto"
    "$compiler" --check "$work/method.atto" > "$work/method.out" 2>"$work/method.err" || {
      printf 'class method rejected marker=%s separator=%s\n' "$marker" "$separator" >&2
      cat "$work/method.err" >&2
      exit 1
    }
    test ! -s "$work/method.out"
  done
done
# Contextual <- must never consume an ordinary less-than followed by
# unary minus in an expression, even without whitespace.
printf 'fnc main(): bool { return 1<-2; }\n' > "$work/expression.atto"
"$compiler" --check "$work/expression.atto" > "$work/expr.out" 2>"$work/expr.err"
test ! -s "$work/expr.out"
printf 'RETURN_FIRST_LLVM_PARSER_ACCEPTED=12 CLASS_METHODS=6 REJECTED=5 WASM_GUEST_NOT_EXECUTED\n'
