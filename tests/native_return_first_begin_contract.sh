#!/bin/sh
# Production LLVM compiler parser/IR admission test, NOT a Wasm guest receipt.
# The private-source CI carrier must be independently approved before mirroring.
set -eu
compiler=${1:?expected native compiler binary}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM
positive=0
negative=0
for header in 'int <- main()' 'int• main()' 'fnc int <- main()' 'fnc int• main()'; do
  for form in braced begin_inline begin_multiline bare bare_semicolon bare_dot; do
    case "$form" in
      braced) printf '%s { return 42; }\n' "$header" ;;
      begin_inline) printf '%s begin return 42; end\n' "$header" ;;
      begin_multiline) printf '%s\nbegin\n  return 42;\nend\n' "$header" ;;
      bare) printf '%s return 42; end\n' "$header" ;;
      bare_semicolon) printf '%s return 42; end;\n' "$header" ;;
      bare_dot) printf '%s\nbegin\n  return 42;\nend.\n' "$header" ;;
    esac > "$work/positive.atto"
    if ! "$compiler" --target=wasm32 "$work/positive.atto" >"$work/out.ll" 2>"$work/error"; then
      printf 'return-first IR admission failed: %s / %s\n' "$header" "$form" >&2
      cat "$work/error" >&2
      exit 1
    fi
    if ! grep -Eq '^[[:space:]]*define[[:space:]].*@' "$work/out.ll"; then
      printf 'return-first produced no defined LLVM function: %s / %s\n' "$header" "$form" >&2
      exit 1
    fi
    positive=$((positive+1))
  done
done
# These must fail without producing even a partial LLVM IR artifact.
for case in 1 2 3 4 5 6 7 8; do
  case "$case" in
    1) printf 'int < - main() { return 42; }\n' ;;
    2) printf 'int <- main()\nbegin\nreturn 42;\nend..\n' ;;
    3) printf 'int <- main() begin return 42;\n' ;;
    4) printf 'fnc int <- main() -> int begin return 42; end\n' ;;
    5) printf 'int <- main() begin 42; end\n' ;;
    6) printf 'int <- main() :: return 42; end\n' ;;
    7) printf 'int <- main() { return 42;\n' ;;
    8) printf 'int ∙ main() { return 42; }\n' ;;
  esac >"$work/negative.atto"
  if "$compiler" --target=wasm32 "$work/negative.atto" >"$work/out.ll" 2>"$work/error"; then
    printf 'malformed return-first accepted: case %s\n' "$case" >&2
    exit 1
  fi
  if [ -s "$work/out.ll" ]; then
    printf 'malformed return-first leaked LLVM IR: case %s\n' "$case" >&2
    exit 1
  fi
  negative=$((negative+1))
done
# Receiver-bearing class methods remain a distinct frontend-only admission.
methods=0
for marker in '' 'mtd ' 'fnc '; do
  for separator in '<-' '•'; do
    for form in braced begin_inline bare; do
      case "$form" in
        braced) body='{ return 42; }' ;;
        begin_inline) body='begin return 42; end' ;;
        bare) body='return 42; end' ;;
      esac
      {
        printf 'define class Box as\n  %sint %s value() %s\nend\n' "$marker" "$separator" "$body"
        printf 'fnc main(): int { return 42; }\n'
      } >"$work/method.atto"
      if ! "$compiler" --check "$work/method.atto" >"$work/method.out" 2>"$work/method.err"; then
        printf 'method rejected marker=%s separator=%s form=%s\n' "$marker" "$separator" "$form" >&2
        cat "$work/method.err" >&2
        exit 1
      fi
      test ! -s "$work/method.out"
      methods=$((methods+1))
    done
  done
done
# Contextual <- must not consume less-than plus unary minus in an expression.
printf 'fnc main(): bool { return 1<-2; }\n' >"$work/expression.atto"
"$compiler" --check "$work/expression.atto" >"$work/expr.out" 2>"$work/expr.err"
test ! -s "$work/expr.out"
printf 'RETURN_FIRST_LLVM_IR_ADMISSION=%s CLASS_METHOD_CHECK=%s NEGATIVE_NO_IR=%s WASM_GUEST_NOT_EXECUTED\n' "$positive" "$methods" "$negative"
