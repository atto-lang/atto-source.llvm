#!/bin/sh
# Frontend-only CLI contract: invocation tests, not typed-effect parity.
set -eu
if [ "$#" -ne 2 ]; then
  echo "usage: $0 /path/to/oreslang-llvmc /path/to/fixtures" >&2
  exit 2
fi
compiler=$1
fixtures=$2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
cp "$fixtures/cfg-guard-call.ores" "$tmp/good.atto"
printf 'fnc main(): int { return ;\n' > "$tmp/bad.atto"

# The real native parser accepts a supported-source fixture with no IR output.
"$compiler" --check "$tmp/good.atto" > "$tmp/out" 2> "$tmp/err"
if [ -s "$tmp/out" ]; then
  echo "--check unexpectedly emitted compiler output" >&2
  exit 1
fi

# Bad source must not turn into a successful no-codegen check.
if "$compiler" --check "$tmp/bad.atto" > "$tmp/out" 2> "$tmp/err"; then
  echo "--check accepted invalid source" >&2
  exit 1
fi

# Compiler effects are NOT implemented: fail closed, even on valid input.
if "$compiler" --analyze "$tmp/good.atto" > "$tmp/out" 2> "$tmp/err"; then
  echo "--analyze falsely certified unsupported effects" >&2
  exit 1
fi
grep -q 'typed throws/raises/panics' "$tmp/err"

# Ensure incompatible mixed modes do not bypass the no-codegen boundary.
if "$compiler" --check --rt-check "$tmp/good.atto" > "$tmp/out" 2> "$tmp/err"; then
  echo "--check combined with --rt-check" >&2
  exit 1
fi
if "$compiler" --check --tree-shake "$tmp/good.atto" > "$tmp/out" 2> "$tmp/err"; then
  echo "--check combined with --tree-shake" >&2
  exit 1
fi


# The --check path is a real parser/semantic admission, not a regex scan.
# Exception-control lowering is not supported even if an effect is declared.
for effect in throw raise panic; do
  printf 'fnc main(): int { %s 42; return 0; }\n' "$effect" > "$tmp/unsupported.atto"
  if "$compiler" --check "$tmp/unsupported.atto" > "$tmp/out" 2> "$tmp/err"; then
    echo "--check falsely admitted unsupported $effect" >&2
    exit 1
  fi
done

# Existing native 'using' metadata cannot stand in for transitive effect inference.
printf 'fnc helper(): int { return 1; }\nfnc main(): int using throw { return helper(); }\n' \
  > "$tmp/transitive.atto"
if "$compiler" --check "$tmp/transitive.atto" > "$tmp/out" 2> "$tmp/err"; then
  echo "--check falsely accepted effects with unproven call summaries" >&2
  exit 1
fi

# The source-admission mode must not create a guest-code artifact.
if find "$tmp" -type f \( -name '*.ll' -o -name '*.bc' -o -name '*.o' -o -name '*.wasm' \) \
    | grep .; then
  echo "--check emitted a codegen artifact" >&2
  exit 1
fi

# All input paths remain byte-identical.
cmp "$fixtures/cfg-guard-call.ores" "$tmp/good.atto"
printf 'fnc main(): int { return ;\n' | cmp - "$tmp/bad.atto"
echo "frontend-only LLVM admission smoke checks passed"
