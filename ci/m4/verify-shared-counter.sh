#!/usr/bin/env bash
set -euo pipefail
repo="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$repo"
expected="$EXPECTED_CARRIER_HEAD_SHA"
if [[ ! "$expected" =~ ^[0-9a-f]{40}$ ]] || [[ "$(git rev-parse HEAD)" != "$expected" ]]; then
  echo 'M4_LLVM_NOT_EXECUTED: exact carrier commit required' >&2; exit 2
fi
git diff --quiet HEAD -- || { echo 'M4_LLVM_NOT_EXECUTED: dirty checkout' >&2; exit 2; }
fixture=fixtures/m4/shared_actor_counter.atto
source_blob="$(git hash-object "$fixture")"
if [[ "$source_blob" != "0f9aca24deb195d7f604a1bfb944e237629803f4" ]]; then
  echo 'M4_LLVM_NOT_EXECUTED: cross-backend source Git blob differs' >&2; exit 2
fi
echo "ATTO_M4_IDENTICAL_SOURCE_GIT_BLOB=$source_blob"
build="build/atto-tenant-full-source"
compiler="$repo/$build/compiler/oreslang-llvmc"
test -x "$compiler" || { echo 'M4_LLVM_NOT_EXECUTED: built LLVM source compiler missing' >&2; exit 2; }
cache="$build/tenant-vm/CMakeCache.txt"
test -s "$cache" || { echo 'M4_LLVM_NOT_EXECUTED: native tenant runtime build missing' >&2; exit 2; }
wasm_rt_include="$(sed -n 's/^WASM_RT_INCLUDE:PATH=//p' "$cache")"
if [[ -z "$wasm_rt_include" || ! -f "$wasm_rt_include/wasm-rt.h" ]]; then
  echo 'M4_LLVM_NOT_EXECUTED: WABT native runtime headers missing' >&2; exit 2
fi
for bin in clang clang++ wasm-ld wat2wasm wasm2c python3; do
  command -v "$bin" >/dev/null || { echo "M4_LLVM_NOT_EXECUTED: $bin missing" >&2; exit 2; }
done
out="$build/tenant-vm/lowered/atto_m4_shared_counter"
test ! -e "$out" || { echo 'M4_LLVM_NOT_EXECUTED: refusing pre-existing guest IR/library' >&2; exit 2; }
python3 tools/tenant-vm/lower_guest.py "$fixture" --out "$out" \
  --include "$repo/src/main/cpp" \
  --adapter "$repo/tools/tenant-vm/guest_adapter.c" \
  --wasm-rt-include "$wasm_rt_include" \
  --compiler "$compiler" --clang "$(command -v clang)" \
  --wasm-clang "$(command -v clang)" --wasm-ld "$(command -v wasm-ld)" \
  --wat2wasm "$(command -v wat2wasm)" --wasm2c "$(command -v wasm2c)"
python3 - "$out" "$fixture" <<'PY'
import hashlib,json,pathlib,sys
out=pathlib.Path(sys.argv[1]); src=pathlib.Path(sys.argv[2])
m=json.loads((out/"manifest.json").read_text())
if m.get("schema")!="ores-tenant-vm-guest-v1":
    raise SystemExit("M4_LLVM_NOT_EXECUTED: wrong guest ABI")
if m.get("input")!=src.name or m.get("input_sha256")!=hashlib.sha256(src.read_bytes()).hexdigest():
    raise SystemExit("M4_LLVM_NOT_EXECUTED: wrong .atto source fixture")
if m.get("mode")!="bounds" or m.get("test_defines"):
    raise SystemExit("M4_LLVM_NOT_EXECUTED: unsafe/mutated guest build")
if m.get("kinds")!={"actors":1,"main":1}:
    raise SystemExit("M4_LLVM_NOT_EXECUTED: wrong compiled source kinds")
binary=out/m["library"]
if not binary.is_file() or m.get("sha256")!=hashlib.sha256(binary.read_bytes()).hexdigest():
    raise SystemExit("M4_LLVM_NOT_EXECUTED: incorrect compiled native guest binary digest")
print("ATTO_M4_LLVM_GUEST_SOURCE_SHA256="+m["input_sha256"])
print("ATTO_M4_LLVM_NATIVE_GUEST_SHA256="+m["sha256"])
PY
clang++ -std=c++17 -O1 -Wall -Wextra -Werror -pthread \
  -I "$repo/src/main/cpp" ci/m4/native_actor_observation.cpp \
  src/main/cpp/oreslang/runtime/TenantVm.cpp \
  src/main/cpp/oreslang/runtime/TenantVmGuestLoader.cpp \
  -ldl -o "$build/m4-native-actor-observation"
library="$(python3 - "$out/manifest.json" <<'PY'
import json,sys
print(json.load(open(sys.argv[1]))["library"])
PY
)"
sha="$(python3 - "$out/manifest.json" <<'PY'
import json,sys
print(json.load(open(sys.argv[1]))["sha256"])
PY
)"
"$build/m4-native-actor-observation" "$out/$library" "$sha" | tee "$build/m4-observations.txt"
for expected in \
  ATTO_M4_LLVM_SOURCE_GUEST_RETURN=43 \
  ATTO_M4_LLVM_ACTOR_CALLS_OBSERVED=3 \
  ATTO_M4_LLVM_OBSERVATION_ACTOR_STOPS=2 \
  ATTO_M4_LLVM_HOST_OBSERVED_ACTOR_DIGEST=7021 \
  ATTO_M4_LLVM_MAIN_GUEST_DRAINED=1 \
  ATTO_M4_LLVM_STALE_HANDLE_DENIED=1 \
  ATTO_M4_LLVM_REINCARNATION_FRESH_STATE=1 \
  ATTO_M4_LLVM_TENANT_NEIGHBOR_SURVIVED=1; do
  grep -Fx "$expected" "$build/m4-observations.txt" || exit 1
done
# The same native loader must fail closed for a wrong signed-off digest and
# for a tampered executable even with the previously verified digest.
deny_native_library() {
  local candidate="$1" expected_sha="$2" reason="$3"
  if "$build/m4-native-actor-observation" "$candidate" "$expected_sha" \
       > "$build/m4-refused-stdout" 2> "$build/m4-refused-stderr"; then
    echo "M4_LLVM_ADMISSION_BYPASS: $reason unexpectedly loaded" >&2; exit 1
  fi
  grep -F 'ATTO_M4_LLVM_GUEST_FAILED: native guest failed exact-digest admission' \
      "$build/m4-refused-stderr" >/dev/null || {
    echo "M4_LLVM_ADMISSION_BYPASS: wrong failure for $reason" >&2
    cat "$build/m4-refused-stderr" >&2
    exit 1
  }
  if grep -q '^ATTO_M4_LLVM_SOURCE_GUEST_RETURN=' "$build/m4-refused-stdout"; then
    echo "M4_LLVM_ADMISSION_BYPASS: untrusted native guest executed" >&2; exit 1
  fi
}
deny_native_library "$out/$library" "$(printf '%064d' 0)" 'wrong digest'
echo 'ATTO_M4_LLVM_NEGATIVE_DIGEST_REJECTION=PASSED'
cp "$out/$library" "$build/m4-corrupted-guest.so"
python3 - "$build/m4-corrupted-guest.so" <<'PY'
import pathlib,sys
path=pathlib.Path(sys.argv[1])
with path.open("r+b") as f:
    f.seek(-1,2)
    byte=f.read(1)
    f.seek(-1,2)
    f.write(bytes([byte[0] ^ 1]))
PY
deny_native_library "$build/m4-corrupted-guest.so" "$sha" 'tampered native image'
echo 'ATTO_M4_LLVM_TAMPERED_GUEST_REJECTION=PASSED'

echo 'ATTO_M4_LLVM_COMPILER_GENERATED_IDENTICAL_SOURCE_GUEST=PASSED'
echo 'EVIDENCE_SCOPE=M4_IDENTICAL_SOURCE_GUEST_EXECUTION_WITH_HOST_OBSERVED_REPLY_TRACE_NOT_FULL_LANGUAGE_OR_SECURITY'
