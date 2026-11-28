#!/usr/bin/env bash
set -euo pipefail
repo="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$repo"
expected="${EXPECTED_CARRIER_HEAD_SHA:-}"
if [[ ! "$expected" =~ ^[0-9a-f]{40}$ ]] || [[ "$(git rev-parse HEAD)" != "$expected" ]]; then
  echo 'M5_CANONICAL_GRAAL_NOT_EXECUTED: exact Git carrier head required' >&2; exit 2
fi
git diff --quiet HEAD -- || { echo 'M5_CANONICAL_GRAAL_NOT_EXECUTED: source checkout changed' >&2; exit 2; }
test -x "${JAVA_HOME:-}/bin/native-image" && test -x "$JAVA_HOME/bin/javac" || {
  echo 'M5_CANONICAL_GRAAL_NOT_EXECUTED: actual GraalVM Native Image unavailable' >&2; exit 2
}
source="fixtures/m4/shared_actor_counter.atto"
if [[ "$(git hash-object "$source")" != "0f9aca24deb195d7f604a1bfb944e237629803f4" ]]; then
  echo 'M5_CANONICAL_GRAAL_NOT_EXECUTED: source fixture Git blob mismatch' >&2; exit 2
fi
# The code generator itself was copied byte-for-byte from the selected
# atto-source.java M5 PR head; never substitute a carrier-local mocked backend.
if [[ "$(git hash-object ci/m5/atto_static_actor_aot.py)" != "77b2bd237552addb3f0eb471609a5519fba32527" ]]; then
  echo 'M5_CANONICAL_GRAAL_NOT_EXECUTED: source codegen Git blob mismatch' >&2; exit 2
fi
compiler="$repo/build/atto-tenant-full-source/compiler/atto-checked-actor-ir"
test -x "$compiler" || { echo 'M5_CANONICAL_GRAAL_NOT_EXECUTED: actual LLVM checked exporter missing' >&2; exit 2; }
exporter_sha="$(sha256sum "$compiler" | cut -d ' ' -f 1)"
source_sha="$(sha256sum "$source" | cut -d ' ' -f 1)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
python3 ci/m5/atto_static_actor_aot.py "$source" \
  --source-sha256 "$source_sha" \
  --checked-exporter "$compiler" \
  --checked-exporter-sha256 "$exporter_sha" \
  --out "$work/checked-java"
python3 - "$work/checked-java" "$source_sha" "$exporter_sha" <<'PY'
import hashlib,json,pathlib,sys
folder=pathlib.Path(sys.argv[1])
m=json.loads((folder/"manifest.json").read_text())
java=(folder/"AttoGeneratedMain.java").read_bytes()
assert m["schema"]=="atto.static-actor-graal-aot-pilot.v1"
assert m["status"]=="GENERATED_SOURCE_NOT_YET_EXECUTED"
assert m["compiler_authority"]=="CANONICAL_LLVM_AST"
assert m["canonical_exporter_sha256"]==sys.argv[3]
assert m["source_sha256"]==sys.argv[2]
assert m["generated_java_sha256"]==hashlib.sha256(java).hexdigest()
assert m["actor_instances"]==2 and m["actor_sends"]==3 and m["expected_guest_result"]==43
print("M5_CANONICAL_EXPORTER_SHA256="+m["canonical_exporter_sha256"])
print("M5_CANONICAL_CHECKED_SOURCE_SHA256="+m["source_sha256"])
print("M5_CANONICAL_CHECKED_JAVA_SHA256="+m["generated_java_sha256"])
PY
mkdir -p "$work/classes"
"$JAVA_HOME/bin/javac" -d "$work/classes" "$work/checked-java/AttoGeneratedMain.java"
(
  cd "$work"
  "$JAVA_HOME/bin/native-image" --no-fallback --gc=epsilon \
    -H:Name=m5-canonical-actor-guest -cp "$work/classes" AttoGeneratedMain
)
test -s "$work/m5-canonical-actor-guest" && test -x "$work/m5-canonical-actor-guest" || {
  echo 'M5_CANONICAL_GRAAL_NOT_EXECUTED: Native Image not produced' >&2; exit 1;
}
file "$work/m5-canonical-actor-guest" | grep -Eq 'ELF|Mach-O' || {
  echo 'M5_CANONICAL_GRAAL_NOT_EXECUTED: not native machine code' >&2; exit 1;
}
echo "M5_CANONICAL_NATIVE_GRAAL_ARTIFACT_SHA256=$(sha256sum "$work/m5-canonical-actor-guest" | cut -d ' ' -f 1)"
if ! timeout 60 "$work/m5-canonical-actor-guest" > "$work/output" 2> "$work/stderr"; then
  echo 'M5_CANONICAL_GRAAL_NATIVE_GUEST_FAILED' >&2
  cat "$work/output" >&2; cat "$work/stderr" >&2; exit 1
fi
cat "$work/output"
if [[ -s "$work/stderr" ]]; then
  echo 'M5_CANONICAL_GRAAL_UNEXPECTED_STDERR' >&2
  cat "$work/stderr" >&2; exit 1
fi
for expected in \
    ATTO_GUEST_NATIVE_AOT_RETURN=43 \
    ATTO_GUEST_ACTOR_SENDS=3 \
    ATTO_GUEST_ACTOR_CLOSES=2 \
    ATTO_GUEST_ACTOR_STATE_DIGEST=7021 \
    ATTO_GUEST_GC_DELTA=0; do
  grep -Fx "$expected" "$work/output" > /dev/null || {
    echo "M5_CANONICAL_GRAAL_MISSING_OBSERVATION=$expected" >&2; exit 1
  }
done
echo 'M5_LLVM_CHECKED_AST_TO_GRAAL_NATIVE_IMAGE_GUEST=PASSED'
echo 'EVIDENCE_SCOPE=CANONICAL_LLVM_FRONTEND_NARROW_STATIC_GRAAL_AOT_NOT_FULL_LANGUAGE_OR_ASYNC'
