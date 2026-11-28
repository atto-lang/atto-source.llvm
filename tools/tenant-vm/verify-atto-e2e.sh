#!/usr/bin/env bash
# Full source-to-native tenant-VM proof, not a host-only / pre-generated-IR test.
set -euo pipefail
repo="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$repo"
expected="${ATTO_E2E_EXPECTED_HEAD_SHA:-}"
if [[ -z "$expected" || ! "$expected" =~ ^[0-9a-f]{40}$ ]]; then
  echo "ATTO_E2E_NOT_EXECUTED: supply exact ATTO_E2E_EXPECTED_HEAD_SHA" >&2; exit 2
fi
test "$(git rev-parse HEAD)" = "$expected" || {
  echo "ATTO_E2E_NOT_EXECUTED: exact source head mismatch" >&2; exit 2;
}
git diff --quiet HEAD -- || {
  echo "ATTO_E2E_NOT_EXECUTED: source checkout was modified" >&2; exit 2;
}
for binary in cmake clang clang++ wasm-ld wasm2c wat2wasm python3 sha256sum file; do
  command -v "$binary" >/dev/null || {
    echo "ATTO_E2E_NOT_EXECUTED: missing $binary" >&2; exit 2;
  }
done
: "${LLVM_DIR:?ATTO_E2E_NOT_EXECUTED: set LLVM_DIR for exact source compiler build}"
build="${ATTO_E2E_BUILD_DIR:-build/atto-tenant-full-source}"
# Avoid ambiguous compiler binaries fetched from elsewhere: build from this checkout.
cmake -S . -B "$build/compiler" -DCMAKE_BUILD_TYPE=Release -DLLVM_DIR="$LLVM_DIR"
cmake --build "$build/compiler" --target oreslang-llvmc -j"${ATTO_BUILD_JOBS:-2}"
compiler="$repo/$build/compiler/oreslang-llvmc"
test -s "$compiler" && test -x "$compiler" || {
  echo "ATTO_E2E_NOT_EXECUTED: compiler not produced by this checkout" >&2; exit 1;
}
compiler_kind="$(file -b "$compiler")"
case "$compiler_kind" in
  *ELF*|*Mach-O*) ;;
  *) echo "ATTO_E2E_NOT_EXECUTED: compiler is not a native executable" >&2; exit 1 ;;
esac
cmake -S tests/tenant-vm -B "$build/tenant-vm" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DTENANT_VM_ATTO_COMPILER="$compiler" -DTENANT_VM_REQUIRE_ATTO_GUESTS=ON
cmake --build "$build/tenant-vm" --target tenant_vm_lowered_test -j"${ATTO_BUILD_JOBS:-2}"
ctest --test-dir "$build/tenant-vm" --output-on-failure \
  -R '^tenant_vm_source_(main|fleet|isolation)$'
ctest --test-dir "$build/tenant-vm" --output-on-failure -R '^tenant_vm_lowered_test$'
ctest --test-dir "$build/tenant-vm" --output-on-failure -R '^tenant_vm_admission_test$'
python3 - "$build/tenant-vm/lowered" <<'PY'
import hashlib, json, pathlib, sys
generated=pathlib.Path(sys.argv[1])
origin=pathlib.Path('tests/tenant-vm/lowered/atto')
for case in ('counter','doubler','spin','recurse','work','fleet','misordered','pair'):
    guest=generated / ('atto_'+case)
    m=json.loads((guest / 'manifest.json').read_text())
    src=origin / (case+'.atto')
    if m.get('schema') != 'ores-tenant-vm-guest-v1':
        raise SystemExit('ATTO_E2E_NOT_EXECUTED: wrong ABI manifest '+case)
    if m.get('input') != src.name:
        raise SystemExit('ATTO_E2E_NOT_EXECUTED: precompiled IR substituted for source '+case)
    if m.get('input_sha256') != hashlib.sha256(src.read_bytes()).hexdigest():
        raise SystemExit('ATTO_E2E_NOT_EXECUTED: wrong exact guest fixture '+case)
    if m.get('mode') != 'bounds' or m.get('test_defines'):
        raise SystemExit('ATTO_E2E_NOT_EXECUTED: unsafe/modified guest profile '+case)
    lib=guest / m['library']
    if m.get('sha256') != hashlib.sha256(lib.read_bytes()).hexdigest():
        raise SystemExit('ATTO_E2E_NOT_EXECUTED: guest artifact SHA mismatch '+case)
    if m.get('kinds',{}).get('main') is None:
        raise SystemExit('ATTO_E2E_NOT_EXECUTED: no compiled main '+case)
    print('ATTO_NATIVE_SOURCE_GUEST_VERIFIED='+case+' sha256='+m['sha256'])
print('ATTO_SOURCE_COMPILED_TENANT_VM=PASSED; LIMIT=NARROW_ACTOR_SUBSET_NOT_FULL_BACKEND_PARITY')
PY
