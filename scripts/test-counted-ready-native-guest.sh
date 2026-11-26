#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root"
compiler="$(realpath "$1")"
corpus="$(realpath "$2")"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
python3 - "$corpus" <<'PY'
import json,sys,pathlib,hashlib
root=pathlib.Path(sys.argv[1])
cases=[c for c in json.loads((root/'manifest.json').read_text())['cases'] if c['id'].startswith(('counted-','reject-counted-'))]
assert len(cases)==16
for c in cases:
    raw=(root/c['path']).read_bytes()
    assert hashlib.sha1(b'blob '+str(len(raw)).encode()+b'\0'+raw).hexdigest()==c['gitBlobSha1']
PY
for name in counted-three counted-zero counted-one counted-alias counted-dynamic counted-dynamic-zero counted-mixed-future-alias counted-multi-channel-fair counted-mixed-zero; do
  "$compiler" --counted-ready-v1 "$corpus/$name.atto" > "$work/guest.cpp"
  clang++ -std=c++17 -O1 -pthread -I src/main/cpp -S -emit-llvm "$work/guest.cpp" -o "$work/guest.ll"
  clang++ -O1 -pthread "$work/guest.ll" -o "$work/guest"
  test "$("$work/guest")" = 42
  echo "GUEST_PASS $name"
done
for name in reject-counted-negative reject-counted-bool reject-counted-scalar reject-counted-default reject-counted-write reject-counted-dynamic-bool reject-counted-dynamic-unknown; do
  if "$compiler" --counted-ready-v1 "$corpus/$name.atto" > "$work/denial" 2> "$work/err"; then exit 1; fi
  test ! -s "$work/denial"
  grep -F E_COUNTED_READY_PROFILE "$work/err"
done
echo LLVM_COUNTED_READY_NATIVE_GUEST_PASS
