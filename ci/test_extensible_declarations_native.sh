#!/usr/bin/env bash
# Execute real compiler-emitted LLVM IR in a source-free directory.
set -euo pipefail
compiler="$(realpath "${1:?compiler required}")"
clang="$(realpath "$(command -v "${2:-clang}")")"
scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT
mkdir -p "$scratch/build" "$scratch/deploy"
cd "$scratch/build"
cat > ordinary.atto <<'ATTO'
define class Customer as
  pub final int token: 42;
end
pub fnc main(): int {
  let Customer value = new Customer();
  return value.token;
}
ATTO
{ printf 'define keyword entity as class end\n'; sed 's/^define class Customer/define entity Customer/' ordinary.atto; } > custom.atto
{ sed 's/^define class Customer/define entity Customer/' ordinary.atto; printf '\ndefine keyword entity as class end\n'; } > forward.atto
for case in ordinary custom forward; do
  "$compiler" "$case.atto" > "$case.ll"
  test -s "$case.ll"
done
cmp ordinary.ll custom.ll
cmp ordinary.ll forward.ll
for opt in 0 2; do
  for case in ordinary custom forward; do
    binary="$scratch/deploy/$case-O$opt"
    "$clang" "-O$opt" -x ir "$case.ll" -o "$binary"
    file "$binary" | grep -E 'ELF|Mach-O'
    sha256sum "$case.atto" "$case.ll" "$binary"
    set +e
    (cd "$scratch/deploy"; env -i PATH=/usr/bin:/bin "./$case-O$opt") > "$case.stdout" 2> "$case.stderr"
    result=$?
    set -e
    test "$result" = 42
    test ! -s "$case.stdout"
    test ! -s "$case.stderr"
    echo "EXTENSIBLE_NATIVE_GUEST_PASS case=$case opt=O$opt exit=42"
  done
  cmp "$scratch/deploy/ordinary-O$opt" "$scratch/deploy/custom-O$opt"
  cmp "$scratch/deploy/ordinary-O$opt" "$scratch/deploy/forward-O$opt"
  echo "EXTENSIBLE_IDENTICAL_IR_AND_BINARY opt=O$opt"
done
reject() {
  local name="$1" diagnostic="$2"
  if "$compiler" "$name.atto" > "$name.ll" 2> "$name.err"; then
    echo "unexpected admission: $name" >&2; exit 1
  fi
  test ! -s "$name.ll"
  grep -F "$diagnostic" "$name.err"
  echo "EXTENSIBLE_NATIVE_NO_IR_REJECT case=$name"
}
printf 'define keyword entity as class end\ndefine keyword entity as class end\n' > duplicate.atto
reject duplicate NATIVE_KIND_DUPLICATE
printf 'define keyword entity as actor end\n' > actor.atto
reject actor NATIVE_KIND_BASE_UNSUPPORTED
printf 'define keyword entity as class requires Entity; end\n' > contract.atto
reject contract NATIVE_KIND_CONTRACT_UNSUPPORTED
printf 'define keyword workflow as class end\n' > reserved.atto
reject reserved NATIVE_KIND_NAME_UNSUPPORTED
printf 'define keyword entity as class end\ndefine module M as end\n' > module.atto
reject module NATIVE_KIND_SCOPE_UNSUPPORTED
# Ordinary visibility checker must still reject a private field read.
sed 's/pub final int token/final int token/' custom.atto > private.atto
reject private private
for kind in http serial macro state service Map; do
  printf 'define keyword %s as class end\n' "$kind" > "reserved-$kind.atto"
  reject "reserved-$kind" NATIVE_KIND_NAME_UNSUPPORTED
done
printf 'define entity Customer as end\ndefine module M as define keyword entity as class end end\n' > nested-forward.atto
reject nested-forward "oreslang-llvmc:"
printf 'define entity Customer as end\ndefine keyword entity as class end\ndefine keyword entity as class end\n' > duplicate-forward.atto
reject duplicate-forward NATIVE_KIND_DUPLICATE
echo 'EXTENSIBLE_LLVM_STANDALONE_NATIVE_GATE_PASS positives=6 negatives=14'
