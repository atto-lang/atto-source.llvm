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
{ printf 'define keyword kind_entity as class end\n'; sed 's/^define class Customer/define kind_entity Customer/' ordinary.atto; } > custom.atto
for case in ordinary custom; do
  "$compiler" "$case.atto" > "$case.ll"
  test -s "$case.ll"
  "$clang" -O1 -x ir "$case.ll" -o "$scratch/deploy/$case"
  file "$scratch/deploy/$case" | grep -E 'ELF|Mach-O'
  sha256sum "$case.atto" "$case.ll" "$scratch/deploy/$case"
done
# Native artifacts alone are copied to deployment; no source, compiler,
# interpreter or IR is present in that directory.
for case in ordinary custom; do
  set +e
  (cd "$scratch/deploy"; env -i PATH=/usr/bin:/bin "./$case") > "$case.stdout" 2> "$case.stderr"
  result=$?
  set -e
  test "$result" = 42
  test ! -s "$case.stdout"
  test ! -s "$case.stderr"
  echo "EXTENSIBLE_NATIVE_GUEST_PASS case=$case exit=42"
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
printf 'define keyword kind_entity as class end\ndefine keyword kind_entity as class end\n' > duplicate.atto
reject duplicate NATIVE_KIND_DUPLICATE
printf 'define keyword kind_entity as actor end\n' > actor.atto
reject actor NATIVE_KIND_BASE_UNSUPPORTED
printf 'define keyword kind_entity as class requires Entity; end\n' > contract.atto
reject contract NATIVE_KIND_CONTRACT_UNSUPPORTED
printf 'define keyword workflow as class end\n' > reserved.atto
reject reserved NATIVE_KIND_NAME_UNSUPPORTED
printf 'define keyword kind_entity as class end\ndefine module M as end\n' > module.atto
reject module NATIVE_KIND_SCOPE_UNSUPPORTED
# Ordinary visibility checker must still reject a private field read.
sed 's/pub final int token/final int token/' custom.atto > private.atto
reject private private
echo 'EXTENSIBLE_LLVM_STANDALONE_NATIVE_GATE_PASS positives=2 negatives=6'
