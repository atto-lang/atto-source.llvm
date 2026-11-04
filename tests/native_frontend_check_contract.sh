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

# All input paths remain byte-identical.
cmp "$fixtures/cfg-guard-call.ores" "$tmp/good.atto"
printf 'fnc main(): int { return ;\n' | cmp - "$tmp/bad.atto"
echo "frontend-only LLVM admission smoke checks passed"
