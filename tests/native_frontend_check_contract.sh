#!/bin/sh
# Production LLVM CLI admission contract. Requires a compiled real oreslang-llvmc.
# Success is FRONTEND ONLY; this script does not prove effects/guest lowering.
set -eu

if [ "$#" -ne 2 ]; then
  echo "usage: $0 /path/to/oreslang-llvmc /path/to/fixtures" >&2
  exit 2
fi
compiler=$1
fixtures=$2
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

# Prevent an unexpected child crash/termination from passing a negative test.
expect_reject() {
  expected_status=$1
  diagnostic=$2
  shift 2
  set +e
  "$@" > "$tmp/out" 2> "$tmp/err"
  actual_status=$?
  set -e
  if [ "$actual_status" -ne "$expected_status" ]; then
    echo "expected exit $expected_status, got $actual_status: $*" >&2
    cat "$tmp/err" >&2
    exit 1
  fi
  if ! grep -Fq -- "$diagnostic" "$tmp/err"; then
    echo "missing diagnostic '$diagnostic': $*" >&2
    cat "$tmp/err" >&2
    exit 1
  fi
  if [ -s "$tmp/out" ]; then
    echo "rejected invocation unexpectedly emitted stdout: $*" >&2
    exit 1
  fi
}

cp "$fixtures/cfg-guard-call.ores" "$tmp/good.atto"
printf 'fnc main(): int { return ;\n' > "$tmp/bad.atto"

# The real native parser must accept both canonical and legacy extensions;
# never treat source-admission stdout as IR output.
"$compiler" --check "$tmp/good.atto" > "$tmp/out" 2> "$tmp/err"
test ! -s "$tmp/out"
"$compiler" --check "$fixtures/cfg-guard-call.ores" > "$tmp/out" 2> "$tmp/err"
test ! -s "$tmp/out"
ln -s good.atto "$tmp/alias.atto"
"$compiler" --check "$tmp/alias.atto" > "$tmp/out" 2> "$tmp/err"
test ! -s "$tmp/out"

# Negative tests must reject through a normal compiler exit, NOT a crash.
expect_reject 1 'oreslang-llvmc:' "$compiler" --check "$tmp/bad.atto"
expect_reject 2 'typed throws/raises/panics' "$compiler" --analyze "$tmp/good.atto"
expect_reject 2 '--check is frontend-only' "$compiler" --check --rt-check "$tmp/good.atto"
expect_reject 2 '--check is frontend-only' "$compiler" --check --tree-shake "$tmp/good.atto"
expect_reject 2 '--check is frontend-only' "$compiler" --check --constraint-descriptors "$tmp/good.atto"
expect_reject 2 'duplicate check option' "$compiler" --check --check "$tmp/good.atto"

# Reject executable exceptional syntax until typed unwind/cleanup lowering exists.
for effect in throw raise panic; do
  printf 'fnc main(): int { %s 42; return 0; }\n' "$effect" > "$tmp/unsupported.atto"
  expect_reject 1 'native exceptional-control boundary unsupported' \
    "$compiler" --check "$tmp/unsupported.atto"
done

# A declared signature cannot authorize an unproven transitive call.
printf 'fnc helper(): int { return 1; }\nfnc main(): int using io { return helper(); }\n' \
  > "$tmp/transitive.atto"
expect_reject 1 'using effects require transitive call summaries' \
  "$compiler" --check "$tmp/transitive.atto"

# Production source-input security admission: no block/char devices, NUL,
# malformed UTF-8, or inputs exceeding the native 8 MiB source limit.
mkfifo "$tmp/nonregular.atto"
expect_reject 1 'source must be a regular file' "$compiler" --check "$tmp/nonregular.atto"
printf 'fnc main(): int { return 1; }\000' > "$tmp/nul.atto"
expect_reject 1 'native source contains NUL byte' "$compiler" --check "$tmp/nul.atto"
printf 'fnc main(): int { return 1; }\377' > "$tmp/invalid-utf8.atto"
expect_reject 1 'native source is not valid UTF-8' "$compiler" --check "$tmp/invalid-utf8.atto"
dd if=/dev/zero of="$tmp/oversize.atto" bs=1048576 count=8 2>/dev/null
printf 'x' >> "$tmp/oversize.atto"
expect_reject 1 'native source exceeds 8 MiB admission limit' \
  "$compiler" --check "$tmp/oversize.atto"

# Only the expected input/output and scratch fixtures may exist; no generated
# bitcode, IR, object, library, executable, or wasm artifact is accepted.
if find "$tmp" -type f \( -name '*.ll' -o -name '*.bc' -o -name '*.o' \
     -o -name '*.wasm' -o -name '*.exe' -o -name '*.so' -o -name '*.dylib' \) \
    | grep .; then
  echo "--check emitted a codegen artifact" >&2
  exit 1
fi

# The compiler is read-only: ensure original and negative fixtures survive.
cmp "$fixtures/cfg-guard-call.ores" "$tmp/good.atto"
printf 'fnc main(): int { return ;\n' | cmp - "$tmp/bad.atto"
echo "frontend-only LLVM admission smoke checks passed (NOT effects parity)"
